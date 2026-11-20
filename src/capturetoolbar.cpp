#include "capturetoolbar.h"

#include "config.h"
#include "icons.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QMenu>
#include <QToolButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const char *const kStyle =
    "#toolbar { background: rgba(255,255,255,245); border: 1px solid rgba(0,0,0,50); border-radius: 4px; }"
    "#toolbar QToolButton { border: none; border-radius: 3px; padding: 3px; color: #222; }"
    "#toolbar QToolButton:hover { background: rgba(30,144,255,45); }"
    "#toolbar QToolButton:checked { background: rgba(30,144,255,90); }"
    "#toolbar QToolButton:disabled { color: #aaa; }"
    "#toolbar #separator { background: rgba(0,0,0,40); }";

} // namespace

CaptureToolbar::CaptureToolbar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("toolbar");
    setAttribute(Qt::WA_StyledBackground);
    setCursor(Qt::ArrowCursor);
    setStyleSheet(kStyle);

    m_color = Config::annotationColor();
    m_sizeLevel = Config::annotationSize();
    m_fill = Config::annotationFill();

    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(4, 4, 4, 4);
    rows->setSpacing(4);
    auto *main = new QHBoxLayout;
    main->setSpacing(2);
    rows->addLayout(main);

    const QHash<Tool, std::pair<QString, const char *>> tools = {
        {Tool::Rect, {tr("矩形（Shift：正方形）"), "rect"}},
        {Tool::Ellipse, {tr("椭圆（Shift：圆）"), "ellipse"}},
        {Tool::Line, {tr("直线（Shift：45° 对齐）"), "line"}},
        {Tool::Arrow, {tr("箭头（Shift：45° 对齐）"), "arrow"}},
        {Tool::Ruler, {tr("尺子：量距离，标出像素长度（Shift：45° 对齐）"), "ruler"}},
        {Tool::Pen, {tr("画笔（Shift：直线）"), "pen"}},
        {Tool::Highlighter, {tr("荧光笔（Shift：直线）"), "highlighter"}},
        {Tool::Text, {tr("文字"), "text"}},
        {Tool::Mosaic, {tr("马赛克"), "mosaic"}},
        {Tool::Blur, {tr("模糊"), "blur"}},
        {Tool::Number, {tr("序号"), "number"}},
        {Tool::Eraser, {tr("橡皮擦：点击或划过标注即删除"), "eraser"}},
    };
    for (const Tool tool : kToolOrder) {
        const auto &[name, icon] = tools[tool];
        QStringList keys;
        const auto digit = std::find(std::begin(kToolByDigit), std::end(kToolByDigit), tool);
        if (digit != std::end(kToolByDigit))
            keys << QStringLiteral("Ctrl+%1").arg(digit - std::begin(kToolByDigit));
        const auto shiftDigit = std::find(std::begin(kToolByShiftDigit), std::end(kToolByShiftDigit), tool);
        if (shiftDigit != std::end(kToolByShiftDigit))
            keys << QStringLiteral("Shift+%1").arg(shiftDigit - std::begin(kToolByShiftDigit));
        const QString shortcut = keys.join(QStringLiteral(" / "));
        QToolButton *button = addButton(main, name, shortcut, icon);
        button->setCheckable(true);
        connect(button, &QToolButton::clicked, this, [this, tool] {
            setTool(m_tool == tool ? Tool::None : tool); // clicking the active tool turns it off
            emit toolChanged(m_tool);
        });
        m_toolButtons.append({tool, button});
    }
    QToolButton *redact = addButton(main, tr("一键打码：自动遮盖手机号、邮箱、身份证号、银行卡号、IP"), "Ctrl+D", "redact");
    connect(redact, &QToolButton::clicked, this, &CaptureToolbar::redactRequested);

    addSeparator(main);
    m_undo = addButton(main, tr("撤销"), "Ctrl+Z", "undo");
    connect(m_undo, &QToolButton::clicked, this, &CaptureToolbar::undoRequested);
    m_redo = addButton(main, tr("重做"), "Ctrl+Y", "redo");
    connect(m_redo, &QToolButton::clicked, this, &CaptureToolbar::redoRequested);

    addSeparator(main);
    connect(addButton(main, tr("取消"), "Esc", "cancel"), &QToolButton::clicked, this, &CaptureToolbar::cancelRequested);
    QToolButton *ocr = addButton(main, tr("识别文字"), "Ctrl+O / O", "ocr");
    connect(ocr, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Ocr); });
    QToolButton *longShot = addButton(main, tr("长截图：滚动截取长内容"), "Ctrl+L", "longshot");
    connect(longShot, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::LongShot); });
    QToolButton *record = addButton(main, tr("录屏：GIF / MP4"), "Ctrl+R / Ctrl+Shift+R", "record");
    auto *recordMenu = new QMenu(record);
    recordMenu->addAction(tr("录制 GIF"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                          [this] { emit actionRequested(CaptureAction::RecordGif); });
    recordMenu->addAction(tr("录制 MP4"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this,
                          [this] { emit actionRequested(CaptureAction::RecordMp4); });
    record->setMenu(recordMenu);
    record->setPopupMode(QToolButton::InstantPopup);
    record->setStyleSheet("QToolButton::menu-indicator { image: none; }");
    QToolButton *beautify = addButton(main, tr("美化：背景、圆角、阴影"), "Ctrl+B", "beautify");
    connect(beautify, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Beautify); });
    QToolButton *save = addButton(main, tr("保存（Ctrl+Shift+S：不询问，直接存入保存文件夹）"), "Ctrl+S", "save");
    connect(save, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Save); });
    QToolButton *pin = addButton(main, tr("贴到桌面"), "Ctrl+T", "pin");
    connect(pin, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Pin); });
    m_done = addButton(main, tr("复制到剪贴板"), "Ctrl+C", "done");
    connect(m_done, &QToolButton::clicked, this,
            [this] { emit actionRequested(m_pinEditing ? CaptureAction::Pin : CaptureAction::Copy); });
    m_captureOnly = {ocr, longShot, record, beautify, save, pin};

    m_options = buildOptions();
    rows->addWidget(m_options);
    m_options->hide();
    setUndoState(false, false);
}

QToolButton *CaptureToolbar::addButton(QHBoxLayout *layout, const QString &text, const QString &shortcut,
                                       const char *icon)
{
    auto *button = new QToolButton(this);
    button->setText(text);
    button->setToolTip(shortcut.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, shortcut));
    if (icon) {
        button->setIcon(toolIcon(icon));
        button->setIconSize(QSize(20, 20));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    button->setFocusPolicy(Qt::NoFocus); // keep keyboard focus on the overlay or text editor
    layout->addWidget(button);
    return button;
}

void CaptureToolbar::addSeparator(QHBoxLayout *layout)
{
    auto *line = new QFrame(this);
    line->setObjectName("separator");
    line->setFixedWidth(1);
    layout->addSpacing(2);
    layout->addWidget(line);
    layout->addSpacing(2);
}

QWidget *CaptureToolbar::buildOptions()
{
    auto *options = new QWidget(this);
    auto *layout = new QHBoxLayout(options);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *sizes = new QButtonGroup(options);
    const QString sizeNames[] = {tr("小"), tr("中"), tr("大")};
    for (int level = 0; level < 3; ++level) {
        QToolButton *button = addButton(layout, sizeNames[level], QString());
        button->setToolTip(tr("粗细 / 字号：%1").arg(sizeNames[level]));
        button->setCheckable(true);
        button->setChecked(level == m_sizeLevel);
        sizes->addButton(button);
        m_sizeButtons.append(button);
        connect(button, &QToolButton::clicked, this, [this, level] {
            m_sizeLevel = level;
            Config::setAnnotationSize(level);
            emit styleChanged();
        });
    }
    addSeparator(layout);

    auto *colors = new QButtonGroup(options);
    for (const QColor &color : Config::annotationPalette()) {
        auto *swatch = new QToolButton(options);
        swatch->setCheckable(true);
        swatch->setChecked(color == m_color);
        swatch->setFocusPolicy(Qt::NoFocus);
        swatch->setToolTip(color.name().toUpper());
        // Own style sheet wins over the toolbar's generic button rules.
        swatch->setStyleSheet(QStringLiteral("QToolButton, QToolButton:hover { background: %1; border: 1px solid "
                                             "rgba(0,0,0,80); border-radius: 3px; min-width: 16px; max-width: 16px; "
                                             "min-height: 16px; max-height: 16px; padding: 0; }"
                                             "QToolButton:checked { border: 2px solid #1E90FF; }")
                                  .arg(color.name()));
        colors->addButton(swatch);
        m_swatches.append({color, swatch});
        layout->addWidget(swatch);
        connect(swatch, &QToolButton::clicked, this, [this, color] {
            m_color = color;
            Config::setAnnotationColor(color);
            emit styleChanged();
        });
    }
    addSeparator(layout);

    m_fillButton = addButton(layout, tr("填充：实心矩形 / 椭圆，文字加底色"), QString(), "fill");
    m_fillButton->setIconSize(QSize(16, 16));
    m_fillButton->setCheckable(true);
    m_fillButton->setChecked(m_fill);
    connect(m_fillButton, &QToolButton::toggled, this, [this](bool on) {
        m_fill = on;
        Config::setAnnotationFill(on);
        emit styleChanged();
    });
    layout->addStretch();
    return options;
}

void CaptureToolbar::setTool(Tool tool)
{
    m_tool = tool;
    for (const auto &[buttonTool, button] : std::as_const(m_toolButtons))
        button->setChecked(buttonTool == tool);
    m_options->setVisible(tool != Tool::None || m_optionsForced);
    adjustSize();
}

void CaptureToolbar::setPinEditing(bool pinEditing)
{
    m_pinEditing = pinEditing;
    for (QToolButton *button : std::as_const(m_captureOnly))
        button->setVisible(!pinEditing);
    m_done->setToolTip(pinEditing ? tr("完成标注 (Enter)") : tr("复制到剪贴板 (Ctrl+C)"));
    adjustSize();
}

void CaptureToolbar::setOptionsVisible(bool visible)
{
    m_optionsForced = visible;
    m_options->setVisible(m_tool != Tool::None || visible);
    adjustSize();
}

void CaptureToolbar::showStyle(const QColor &color, int sizeLevel, bool fill)
{
    m_color = color;
    m_sizeLevel = qBound(0, sizeLevel, 2);
    m_fill = fill;
    {
        const QSignalBlocker block(m_fillButton);
        m_fillButton->setChecked(fill);
    }
    for (const auto &[swatchColor, swatch] : std::as_const(m_swatches))
        swatch->setChecked(swatchColor == color);
    for (int i = 0; i < m_sizeButtons.size(); ++i)
        m_sizeButtons[i]->setChecked(i == m_sizeLevel);
}

void CaptureToolbar::setUndoState(bool canUndo, bool canRedo)
{
    m_undo->setEnabled(canUndo);
    m_redo->setEnabled(canRedo);
}
