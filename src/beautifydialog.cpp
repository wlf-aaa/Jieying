#include "beautifydialog.h"

#include "icons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QSlider>
#include <QVBoxLayout>

namespace {

constexpr QSize kPreviewSize(560, 400);

// Transparent areas show as a light checkerboard, as in image editors.
QPixmap previewPixmap(const QImage &image)
{
    const QImage scaled = image.width() > kPreviewSize.width() || image.height() > kPreviewSize.height()
                              ? image.scaled(kPreviewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                              : image;
    QImage canvas(scaled.size(), QImage::Format_ARGB32_Premultiplied);
    QPainter p(&canvas);
    for (int y = 0; y < canvas.height(); y += 8)
        for (int x = 0; x < canvas.width(); x += 8)
            p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(228, 228, 228) : QColor(250, 250, 250));
    QImage pixels = scaled;
    pixels.setDevicePixelRatio(1);
    p.drawImage(0, 0, pixels);
    p.end();
    return QPixmap::fromImage(canvas);
}

} // namespace

BeautifyDialog::BeautifyDialog(const QPixmap &image, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::WindowStaysOnTopHint)
    , m_source(image.toImage())
    , m_options(Beautify::loadOptions())
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("截图美化")); // Qt appends " - 截影"

    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(kPreviewSize);
    m_preview->setStyleSheet("background: #f3f4f6; border-radius: 6px;");

    auto *background = m_background = new QComboBox(this);
    background->addItems(Beautify::backgroundNames());
    background->setCurrentIndex(m_options.background);
    connect(background, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_options.background = index;
        refresh();
    });
    auto slider = [this](int max, int value, auto apply) {
        auto *s = new QSlider(Qt::Horizontal, this);
        s->setRange(0, max);
        s->setValue(value);
        connect(s, &QSlider::valueChanged, this, [this, apply](int v) {
            apply(v);
            refresh();
        });
        return s;
    };
    auto *padding = m_padding = slider(120, m_options.padding, [this](int v) { m_options.padding = v; });
    auto *radius = m_radius = slider(40, m_options.radius, [this](int v) { m_options.radius = v; });
    auto *shadow = m_shadow = new QCheckBox(tr("阴影"), this);
    shadow->setChecked(m_options.shadow);
    connect(shadow, &QCheckBox::toggled, this, [this](bool on) {
        m_options.shadow = on;
        refresh();
    });

    auto *form = new QFormLayout;
    form->addRow(tr("背景："), background);
    form->addRow(tr("边距："), padding);
    form->addRow(tr("圆角："), radius);
    form->addRow(QString(), shadow);

    auto *actions = new QHBoxLayout;
    const std::tuple<QString, CaptureAction, const char *> buttons[] = {
        {tr("复制"), CaptureAction::Copy, "copy"},
        {tr("保存"), CaptureAction::Save, "save"},
        {tr("贴图"), CaptureAction::Pin, "pin"},
    };
    for (const auto &[text, action, icon] : buttons) {
        auto *button = new QPushButton(toolIcon(icon), text, this);
        connect(button, &QPushButton::clicked, this, [this, action] { finish(action); });
        actions->addWidget(button);
    }
    actions->addStretch();
    auto *close = new QPushButton(tr("关闭"), this);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    actions->addWidget(close);
    connect(new QShortcut(Qt::Key_Escape, this), &QShortcut::activated, this, &QWidget::close);
    connect(new QShortcut(QKeySequence::Copy, this), &QShortcut::activated, this,
            [this] { finish(CaptureAction::Copy); });

    auto *side = new QVBoxLayout;
    side->addLayout(form);
    side->addStretch();
    auto *body = new QHBoxLayout;
    body->addWidget(m_preview, 1);
    body->addLayout(side);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addLayout(actions);

    refresh();
}

void BeautifyDialog::setOptions(const Beautify::Options &options)
{
    m_options = options;
    // Keep the controls showing what is rendered.
    const QSignalBlocker b1(m_background), b2(m_padding), b3(m_radius), b4(m_shadow);
    m_background->setCurrentIndex(options.background);
    m_padding->setValue(options.padding);
    m_radius->setValue(options.radius);
    m_shadow->setChecked(options.shadow);
    refresh();
}

void BeautifyDialog::refresh()
{
    m_result = Beautify::render(m_source, m_options);
    m_preview->setPixmap(previewPixmap(m_result));
}

void BeautifyDialog::finish(CaptureAction action)
{
    Beautify::saveOptions(m_options); // the look used last becomes the next default
    const QPixmap image = QPixmap::fromImage(m_result);
    const QPoint topLeft = QCursor::pos() - QPoint(int(image.deviceIndependentSize().width() / 2),
                                                   int(image.deviceIndependentSize().height() / 2));
    emit actionRequested(action, image, topLeft);
    close();
}
