#include "livepindialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <utility>

class LivePinPreview final : public QWidget
{
public:
    using QWidget::QWidget;

    QRect imageRect() const
    {
        if (m_image.isNull() || size().isEmpty())
            return {};
        const QSize fitted = m_image.size().scaled(size(), Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        return QRect(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
    }

    void setImage(QImage image)
    {
        if (image.isNull() && m_image.isNull())
            return;
        m_image = std::move(image);
        update();
    }

    const QImage &image() const { return m_image; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(24, 24, 24));
        if (!m_image.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(imageRect(), m_image);
        }
    }

private:
    QImage m_image;
};

namespace {

constexpr int kPollIntervalMs = 66;
constexpr int kFirstFrameTimeoutMs = 3500;

class WindowsLivePinSource final : public LivePinSource
{
public:
    bool isSupported() override { return WgcWindowGrabber::isSupported(); }
    QList<CaptureWindow> windows() override { return snapshotCaptureWindows(); }
    CaptureWindowState windowState(const CaptureWindow &window) override { return captureWindowState(window); }
    bool start(const CaptureWindow &window) override { return m_grabber.start(window.handle); }
    WindowCaptureFrame poll() override { return m_grabber.poll(); }
    void stop() override { m_grabber.stop(); }
    QString error() const override { return m_grabber.error(); }

private:
    WgcWindowGrabber m_grabber;
};

} // namespace

LivePinDialog::LivePinDialog(QWidget *parent)
    : LivePinDialog(std::make_unique<WindowsLivePinSource>(), parent)
{
}

LivePinDialog::LivePinDialog(std::unique_ptr<LivePinSource> source, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowStaysOnTopHint | Qt::WindowCloseButtonHint)
    , m_source(std::move(source))
    , m_pages(new QStackedWidget(this))
    , m_windows(new QListWidget(m_pages))
    , m_preview(new LivePinPreview(m_pages))
    , m_status(new QLabel(this))
    , m_refresh(new QPushButton(tr("刷新列表"), this))
    , m_start(new QPushButton(tr("开始贴图"), this))
    , m_choose(new QPushButton(tr("重新选择"), this))
    , m_freeze(new QPushButton(tr("冻结为图片"), this))
    , m_supported(m_source->isSupported())
{
    m_windows->setObjectName(QStringLiteral("livePinWindows"));
    m_preview->setObjectName(QStringLiteral("livePinPreview"));
    m_status->setObjectName(QStringLiteral("livePinStatus"));
    m_start->setObjectName(QStringLiteral("livePinStart"));
    m_choose->setObjectName(QStringLiteral("livePinChoose"));
    m_freeze->setObjectName(QStringLiteral("livePinFreeze"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_windows->setAccessibleName(tr("可贴图的窗口"));
    m_preview->setAccessibleName(tr("实时窗口预览"));
    m_preview->setMinimumSize(180, 120);
    m_pages->addWidget(m_windows);
    m_pages->addWidget(m_preview);

    auto *actions = new QHBoxLayout;
    actions->addWidget(m_refresh);
    actions->addWidget(m_choose);
    actions->addStretch();
    actions->addWidget(m_start);
    actions->addWidget(m_freeze);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages, 1);
    layout->addWidget(m_status);
    layout->addLayout(actions);

    connect(m_windows, &QListWidget::currentRowChanged, this, &LivePinDialog::updateSelection);
    connect(m_windows, &QListWidget::itemActivated, this, &LivePinDialog::beginCapture);
    connect(m_refresh, &QPushButton::clicked, this, &LivePinDialog::refreshWindows);
    connect(m_start, &QPushButton::clicked, this, &LivePinDialog::beginCapture);
    connect(m_choose, &QPushButton::clicked, this, &LivePinDialog::chooseWindow);
    connect(m_freeze, &QPushButton::clicked, this, &LivePinDialog::freezeFrame);
    connect(&m_timer, &QTimer::timeout, this, &LivePinDialog::updateCapture);
    m_timer.setInterval(kPollIntervalMs);
    m_freeze->setAutoDefault(false);

    resize(520, 390);
    chooseWindow();
}

void LivePinDialog::chooseWindow()
{
    m_timer.stop();
    stopCapture();
    m_target = {};
    m_pages->setCurrentWidget(m_windows);
    m_start->show();
    m_refresh->show();
    m_choose->hide();
    m_freeze->hide();
    setWindowTitle(tr("实时窗口贴图"));
    refreshWindows();
}

void LivePinDialog::refreshWindows()
{
    const int currentRow = m_windows->currentRow();
    const quintptr previous = currentRow >= 0 && currentRow < m_candidates.size()
        ? m_candidates.at(currentRow).handle : 0;
    m_windows->clear();
    m_candidates = m_supported ? m_source->windows() : QList<CaptureWindow>();
    int row = 0;
    for (qsizetype i = 0; i < m_candidates.size(); ++i) {
        const CaptureWindow &window = m_candidates.at(i);
        auto *entry = new QListWidgetItem(window.title, m_windows);
        entry->setToolTip(QStringLiteral("<qt>%1</qt>").arg(window.title.toHtmlEscaped()));
        if (window.handle == previous)
            row = int(i);
    }
    m_windows->setCurrentRow(m_candidates.isEmpty() ? -1 : row);
    m_refresh->setEnabled(m_supported);
    m_status->setToolTip({});
    if (!m_supported)
        m_status->setText(tr("当前系统不支持实时窗口捕获。"));
    else if (m_candidates.isEmpty())
        m_status->setText(tr("没有可用窗口。请打开要贴图的窗口后刷新。"));
    else
        m_status->setText(tr("选择要实时贴图的窗口。"));
    m_windows->setFocus();
    updateSelection();
}

void LivePinDialog::updateSelection()
{
    const int row = m_windows->currentRow();
    m_start->setEnabled(m_supported && row >= 0 && row < m_candidates.size());
}

void LivePinDialog::beginCapture()
{
    const int row = m_windows->currentRow();
    if (m_pages->currentWidget() != m_windows || !m_start->isEnabled()
        || row < 0 || row >= m_candidates.size())
        return;
    m_target = m_candidates.at(row);
    setWindowTitle(tr("实时窗口贴图 · %1").arg(m_target.title));
    m_pages->setCurrentWidget(m_preview);
    m_start->hide();
    m_refresh->hide();
    m_choose->show();
    m_freeze->show();
    m_timer.start();
    updateCapture();
}

void LivePinDialog::resetFrame()
{
    m_preview->setImage({});
    m_freeze->setEnabled(false);
}

void LivePinDialog::stopCapture()
{
    if (m_capturing)
        m_source->stop();
    m_capturing = false;
    m_waiting.invalidate();
    resetFrame();
}

void LivePinDialog::finishCapture(const QString &message, const QString &detail)
{
    m_timer.stop();
    stopCapture();
    m_target = {};
    m_status->setText(message);
    m_status->setToolTip(detail);
}

void LivePinDialog::updateCapture()
{
    switch (m_source->windowState(m_target)) {
    case CaptureWindowState::Closed:
        finishCapture(tr("原窗口已关闭，请重新选择。"));
        return;
    case CaptureWindowState::Minimized:
        stopCapture();
        m_status->setText(tr("原窗口已最小化，恢复后继续。"));
        return;
    case CaptureWindowState::Unavailable:
        stopCapture();
        m_status->setText(tr("原窗口暂不可用，恢复后继续。"));
        return;
    case CaptureWindowState::Available:
        break;
    }

    if (!m_capturing) {
        if (!m_source->start(m_target)) {
            // The target can disappear while Windows is starting its capture session.
            if (m_source->windowState(m_target) != CaptureWindowState::Available)
                return;
            finishCapture(tr("无法捕获此窗口，请重新选择。"), m_source->error());
            return;
        }
        m_capturing = true;
        m_waiting.start();
        m_status->setText(tr("正在等待窗口画面…"));
    }

    auto next = m_source->poll();
    switch (next.status) {
    case WindowCaptureFrame::Status::Frame:
        m_preview->setImage(std::move(next.image));
        m_waiting.invalidate();
        m_freeze->setEnabled(true);
        m_status->clear();
        return;
    case WindowCaptureFrame::Status::Resizing:
        m_waiting.start();
        resetFrame();
        m_status->setText(tr("正在等待窗口画面…"));
        break;
    case WindowCaptureFrame::Status::Closed:
        finishCapture(tr("原窗口已关闭，请重新选择。"));
        return;
    case WindowCaptureFrame::Status::Error:
        finishCapture(tr("无法继续捕获此窗口，请重新选择。"), m_source->error());
        return;
    case WindowCaptureFrame::Status::NoFrame:
        break;
    }
    if (m_waiting.isValid() && m_waiting.hasExpired(kFirstFrameTimeoutMs))
        finishCapture(tr("未收到窗口画面，请重新选择。"));
}

void LivePinDialog::freezeFrame()
{
    if (m_preview->image().isNull())
        return;
    if (m_source->windowState(m_target) != CaptureWindowState::Available) {
        updateCapture();
        return;
    }
    QPixmap pixmap = QPixmap::fromImage(m_preview->image());
    if (pixmap.isNull()) {
        m_status->setText(tr("无法冻结当前画面。"));
        return;
    }
    pixmap.setDevicePixelRatio(m_preview->devicePixelRatioF());
    const QRect displayRect = m_preview->imageRect();
    const QSizeF sourceSize = pixmap.deviceIndependentSize();
    // Clamp the painted size to at least one pixel for thin windows. Use the
    // limiting dimension to carry the same aspect ratio across the handoff.
    const qreal scale = qMin(displayRect.width() / sourceSize.width(),
                            displayRect.height() / sourceSize.height());
    const QPoint globalOrigin = m_preview->mapToGlobal(displayRect.topLeft());
    const QPointer<LivePinDialog> self(this);
    accept();
    if (self)
        emit freezeRequested(pixmap, globalOrigin, scale);
}

void LivePinDialog::done(int result)
{
    m_timer.stop();
    stopCapture();
    QDialog::done(result);
}
