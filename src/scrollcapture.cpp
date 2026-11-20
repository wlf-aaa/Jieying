#include "scrollcapture.h"

#include "icons.h"
#include "regionframe.h"
#include "screengrab.h"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QVBoxLayout>

#include <windows.h>

namespace {
constexpr int kSampleMs = 100;
constexpr int kPreviewWidth = 180;
constexpr int kPreviewHeight = 320;
constexpr int kAutoScrollMs = 350; // one wheel notch per tick; samples run every 100 ms in between

void wheelNotchAt(const QPoint &physicalPos, bool sideways)
{
    SetCursorPos(physicalPos.x(), physicalPos.y());
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = sideways ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = DWORD(sideways ? WHEEL_DELTA : -WHEEL_DELTA); // one notch right / down
    SendInput(1, &input, sizeof(input));
}
} // namespace

int ScrollCapture::maxHeightFor(int width)
{
    // ~240 MB of canvas; the finished image is copied a couple of times on the way out.
    constexpr qint64 kMaxPixels = 60'000'000;
    return int(qBound(qint64(2000), kMaxPixels / qMax(1, width), qint64(30000)));
}

ScrollCapture::ScrollCapture(QScreen *screen, const QRect &physicalRegion, QObject *parent)
    : QObject(parent)
    , m_screen(screen)
    , m_dpr(screen->devicePixelRatio())
    , m_stitcher(maxHeightFor(physicalRegion.width()), maxHeightFor(physicalRegion.height()))
{
    // A screen's logical top-left equals its native one, so it anchors both coordinate systems.
    const QPoint origin = screen->geometry().topLeft();
    m_physical = physicalRegion.translated(origin);
    const QRectF logical(physicalRegion.x() / m_dpr, physicalRegion.y() / m_dpr, physicalRegion.width() / m_dpr,
                         physicalRegion.height() / m_dpr);
    m_logical = logical.toAlignedRect().translated(origin);
    m_timer.setInterval(kSampleMs);
    connect(&m_timer, &QTimer::timeout, this, &ScrollCapture::sample);
    m_autoTimer.setInterval(kAutoScrollMs);
    connect(&m_autoTimer, &QTimer::timeout, this, &ScrollCapture::autoScrollTick);
}

ScrollCapture::~ScrollCapture()
{
    closeWindows();
}

void ScrollCapture::start()
{
    m_frame = new RegionFrame(m_screen, m_logical, QColor(30, 144, 255));
    m_frame->show();
    buildPanel();
    sample();
    m_timer.start();
}

void ScrollCapture::buildPanel()
{
    auto *panel = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    panel->setObjectName("panel");
    panel->setAttribute(Qt::WA_StyledBackground);
    panel->setAttribute(Qt::WA_DeleteOnClose);
    panel->setStyleSheet(kPanelStyle);

    m_preview = new QLabel(panel);
    m_preview->setFixedWidth(kPreviewWidth);
    m_preview->setMinimumHeight(60);
    m_preview->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    m_preview->setStyleSheet("background: #f0f1f3; border: 1px solid rgba(0,0,0,30);");
    m_status = new QLabel(tr("向下或向右滚动要截取的内容\n（滚轮或拖动滚动条）"), panel);
    m_status->setWordWrap(true);
    m_status->setFixedWidth(kPreviewWidth);

    auto *buttons = new QGridLayout;
    buttons->setSpacing(2);
    const std::tuple<QString, CaptureAction, const char *> actions[] = {
        {tr("复制"), CaptureAction::Copy, "copy"},
        {tr("保存"), CaptureAction::Save, "save"},
        {tr("贴图"), CaptureAction::Pin, "pin"},
        {tr("识别文字"), CaptureAction::Ocr, "ocr"},
    };
    int i = 0;
    for (const auto &[text, action, icon] : actions) {
        auto *button = new QPushButton(toolIcon(icon), text, panel);
        connect(button, &QPushButton::clicked, this, [this, action] { finish(action); });
        buttons->addWidget(button, i / 2, i % 2);
        ++i;
    }
    m_autoButton = new QPushButton(toolIcon("longshot"), tr("自动滚动"), panel);
    m_autoButton->setCheckable(true);
    m_autoButton->setToolTip(tr("自动滚动并拼接（已向右滚动过则向右）；移动鼠标即停止"));
    connect(m_autoButton, &QPushButton::toggled, this, [this](bool on) {
        if (on)
            startAutoScroll();
        else
            stopAutoScroll(tr("已停止自动滚动"));
    });
    buttons->addWidget(m_autoButton, 2, 0, 1, 2);
    auto *cancel = new QPushButton(toolIcon("cancel"), tr("取消"), panel);
    connect(cancel, &QPushButton::clicked, this, &ScrollCapture::cancel);
    buttons->addWidget(cancel, 3, 0, 1, 2);
    connect(new QShortcut(Qt::Key_Escape, panel), &QShortcut::activated, this, &ScrollCapture::cancel);
    connect(new QShortcut(Qt::Key_Return, panel), &QShortcut::activated, this,
            [this] { finish(CaptureAction::Copy); });

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_preview);
    layout->addWidget(m_status);
    layout->addLayout(buttons);

    m_panel = panel;
    panel->adjustSize();
    panel->resize(panel->sizeHint().width(), panel->sizeHint().height() + kPreviewHeight - 60);
    panel->move(placeBeside(m_screen, m_logical, panel->size()));
    panel->show();
    excludeFromCapture(panel);
}

void ScrollCapture::sample()
{
    using Status = ScrollStitcher::Status;
    const Status status = m_stitcher.add(grabScreenRect(m_physical, false));
    switch (status) {
    case Status::Started:
    case Status::Appended:
        m_appendedSinceTick = true;
        m_status->setText((m_stitcher.direction() == TwoWayStitcher::Direction::Right ? tr("横向已拼接 %1 px")
                                                                                      : tr("已拼接 %1 px"))
                              .arg(m_stitcher.length())
                          + tr("\n继续滚动，或点击完成方式"));
        break;
    case Status::Lost:
        m_lostSinceTick = true;
        m_status->setText(tr("没有接上：滚动太快或往回滚了，\n请往回滚一点"));
        m_status->setStyleSheet("color: #d93025;");
        return;
    case Status::Full:
        m_full = true;
        m_status->setText(tr("已达到最大长度 %1 px").arg(m_stitcher.length()));
        m_timer.stop();
        break;
    case Status::Unchanged:
        return;
    }
    m_status->setStyleSheet(QString());

    // Preview the newest part of the result (bottom, or right end), scaled into the panel.
    const bool sideways = m_stitcher.direction() == TwoWayStitcher::Direction::Right;
    const qreal scale = qreal(kPreviewWidth - 2) / m_physical.width();
    const QImage tail = m_stitcher.tail(sideways ? 2 * m_physical.width() : int(kPreviewHeight / scale));
    const QImage shown =
        tail.scaled(QSize(kPreviewWidth - 2, kPreviewHeight), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_preview->setPixmap(QPixmap::fromImage(shown));
    m_preview->setFixedHeight(shown.height() + 2);
}

void ScrollCapture::startAutoScroll()
{
    m_autoScroller.reset();
    m_appendedSinceTick = m_lostSinceTick = false;
    m_autoCursor = m_physical.center();
    wheelNotchAt(m_autoCursor, m_stitcher.direction() == TwoWayStitcher::Direction::Right);
    m_autoTimer.start();
    m_status->setText(tr("正在自动滚动…\n移动鼠标即可停止"));
}

void ScrollCapture::autoScrollTick()
{
    POINT cursor{};
    GetCursorPos(&cursor);
    const bool userMoved = (QPoint(cursor.x, cursor.y) - m_autoCursor).manhattanLength() > 3;
    const AutoScroller::Decision decision = m_autoScroller.next(m_appendedSinceTick, m_lostSinceTick, m_full, userMoved);
    m_appendedSinceTick = m_lostSinceTick = false;
    switch (decision) {
    case AutoScroller::Decision::Scroll:
        wheelNotchAt(m_autoCursor, m_stitcher.direction() == TwoWayStitcher::Direction::Right);
        break;
    case AutoScroller::Decision::StopAtEnd:
        stopAutoScroll(tr("已滚动到头（共 %1 px），\n可以选择完成方式").arg(m_stitcher.length()));
        break;
    case AutoScroller::Decision::StopByUser:
        stopAutoScroll(tr("你移动了鼠标，已停止自动滚动"));
        break;
    case AutoScroller::Decision::StopLost:
        stopAutoScroll(tr("选区太矮，一格滚动就超出了选区，\n请往回滚一点后手动慢慢滚动"));
        break;
    case AutoScroller::Decision::StopFull:
        stopAutoScroll(tr("已达到最大长度 %1 px").arg(m_stitcher.length()));
        break;
    }
}

void ScrollCapture::stopAutoScroll(const QString &reason)
{
    if (!m_autoTimer.isActive())
        return;
    m_autoTimer.stop();
    if (m_autoButton) {
        const QSignalBlocker block(m_autoButton);
        m_autoButton->setChecked(false);
    }
    m_status->setText(reason);
}

void ScrollCapture::finish(CaptureAction action)
{
    m_autoTimer.stop();
    m_timer.stop();
    QPixmap image = QPixmap::fromImage(m_stitcher.result());
    image.setDevicePixelRatio(m_dpr);
    closeWindows();
    emit finished(action, image, m_logical.topLeft());
}

void ScrollCapture::cancel()
{
    m_autoTimer.stop();
    m_timer.stop();
    closeWindows();
    emit cancelled();
}

void ScrollCapture::closeWindows()
{
    if (m_frame)
        m_frame->close();
    if (m_panel)
        m_panel->close();
}
