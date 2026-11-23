#include "capturetoolbar.h"

#include "config.h"
#include "icons.h"

#include <QButtonGroup>
#include <QHash>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtAlgorithms>

#include <algorithm>

namespace {

constexpr int kButtonSize = 30;
constexpr auto kStyle = R"(
    #toolbar {
        background: #fbfdff;
        border: 1px solid #b7c5d5;
        border-radius: 10px;
    }
    #toolbar #toolOptions { border-top: 1px solid #e3eaf2; }
    #toolbar #outputPanel {
        background: #eff5fc;
        border: 1px solid #e0eaf5;
        border-radius: 7px;
    }
    #toolbar QToolButton {
        background: transparent;
        border: 1px solid transparent;
        border-radius: 5px;
        color: #26364a;
        padding: 0;
    }
    #toolbar QToolButton:hover:enabled { background: #e8f2fc; }
    #toolbar QToolButton:checked {
        background: #e0efff;
        border-color: #a2caf0;
    }
    #toolbar QToolButton:checked:hover { background: #d5e9fd; }
    #toolbar QToolButton:pressed { background: #cce3fb; }
    #toolbar QToolButton:disabled { color: #8e9aaa; }
    #toolbar QToolButton#done {
        background: #dfedff;
        border-color: #a2caf0;
    }
    #toolbar QToolButton#done:hover { background: #d0e6fd; }
    #toolbar QToolButton#done:pressed { background: #bedcf9; }
    #toolbar QToolButton:focus, #toolbar QToolButton#done:focus { border-color: #1473cf; }
)";

class ToolbarButton : public QToolButton
{
public:
    explicit ToolbarButton(QWidget *parent) : QToolButton(parent)
    {
        setFixedSize(kButtonSize, kButtonSize);
        setFocusPolicy(Qt::TabFocus); // pointer clicks keep focus in the overlay or inline editor
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Let the overlay handle its selection movement and confirmation shortcuts.
            event->ignore();
            return;
        default:
            QToolButton::keyPressEvent(event);
        }
    }
};

class ColorSwatch : public ToolbarButton
{
public:
    ColorSwatch(const QColor &color, QWidget *parent) : ToolbarButton(parent), m_color(color)
    {
        setToolTip(color.name().toUpper());
        setAccessibleName(toolTip());
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ToolbarButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(38, 54, 74, 80));
        painter.setBrush(m_color);
        constexpr qreal diameter = 14.0;
        painter.drawEllipse(QRectF((width() - diameter) / 2, (height() - diameter) / 2, diameter, diameter));
    }

private:
    QColor m_color;
};

// Keeps button sizes and order when a toolbar row needs to wrap.
class ToolbarRow : public QLayout
{
public:
    explicit ToolbarRow(int spacing, QWidget *parent = nullptr) : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(spacing);
    }

    ~ToolbarRow() override { qDeleteAll(m_items); }

    void addItem(QLayoutItem *item) override
    {
        m_items.append(item);
        invalidate();
    }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= m_items.size())
            return nullptr;
        QLayoutItem *item = m_items.takeAt(index);
        invalidate();
        return item;
    }

    QSize minimumSize() const override
    {
        QSize minimum(0, 0);
        for (QLayoutItem *item : m_items) {
            if (!item->isEmpty())
                minimum = minimum.expandedTo(item->sizeHint());
        }
        return minimum.grownBy(contentsMargins());
    }

    QSize sizeHint() const override { return arrange(QWIDGETSIZE_MAX); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(width).height(); }

    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect.width(), &rect);
    }

private:
    QSize arrange(int width, const QRect *geometry = nullptr) const
    {
        const QMargins margins = contentsMargins();
        const int availableWidth = width - margins.left() - margins.right();
        const int rowHeight = minimumSize().shrunkBy(margins).height();
        int rowWidth = 0;
        int widestRow = 0;
        int y = 0;
        for (QLayoutItem *item : m_items) {
            if (item->isEmpty())
                continue;
            const QSize size = item->sizeHint();
            int x = rowWidth > 0 ? rowWidth + spacing() : 0;
            if (x > 0 && x + size.width() > availableWidth) {
                x = 0;
                y += rowHeight + spacing();
            }
            if (geometry) {
                const QPoint offset(x + margins.left(), y + margins.top() + (rowHeight - size.height()) / 2);
                item->setGeometry(QRect(geometry->topLeft() + offset, size));
            }
            rowWidth = x + size.width();
            widestRow = qMax(widestRow, rowWidth);
        }
        return QSize(widestRow, y + rowHeight).grownBy(margins);
    }

    QList<QLayoutItem *> m_items;
};

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
    rows->setContentsMargins(6, 6, 6, 6);
    rows->setSpacing(5);
    auto *annotationRow = new ToolbarRow(2);
    rows->addLayout(annotationRow);

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
        {Tool::Spotlight, {tr("聚光：拖框突出区域，可添加多个"), "spotlight"}},
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
        QToolButton *button = addButton(annotationRow, name, shortcut, icon);
        button->setCheckable(true);
        connect(button, &QToolButton::clicked, this, [this, tool] {
            setTool(m_tool == tool ? Tool::None : tool); // clicking the active tool turns it off
            emit toolChanged(m_tool);
        });
        m_toolButtons.append({tool, button});
    }
    QToolButton *redact = addButton(annotationRow, tr("一键打码：自动遮盖手机号、邮箱、身份证号、银行卡号、IP"),
                                   "Ctrl+D", "redact");
    connect(redact, &QToolButton::clicked, this, &CaptureToolbar::redactRequested);

    m_undo = addButton(annotationRow, tr("撤销"), "Ctrl+Z", "undo");
    connect(m_undo, &QToolButton::clicked, this, &CaptureToolbar::undoRequested);
    m_redo = addButton(annotationRow, tr("重做"), "Ctrl+Y", "redo");
    connect(m_redo, &QToolButton::clicked, this, &CaptureToolbar::redoRequested);

    m_options = buildOptions();
    rows->addWidget(m_options);
    m_options->hide();

    auto *outputPanel = new QWidget(this);
    outputPanel->setObjectName("outputPanel");
    outputPanel->setAttribute(Qt::WA_StyledBackground);
    auto *outputRow = new ToolbarRow(2, outputPanel);
    outputRow->setContentsMargins(4, 4, 4, 4);
    rows->addWidget(outputPanel, 0, Qt::AlignRight);

    connect(addButton(outputRow, tr("取消"), "Esc", "cancel"), &QToolButton::clicked, this,
            &CaptureToolbar::cancelRequested);
    QToolButton *ocr = addButton(outputRow, tr("识别文字"), "Ctrl+O / O", "ocr");
    connect(ocr, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Ocr); });
    QToolButton *longShot = addButton(outputRow, tr("长截图：滚动截取长内容"), "Ctrl+L", "longshot");
    connect(longShot, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::LongShot); });
    QToolButton *record = addButton(outputRow, tr("录屏：GIF / MP4"), "Ctrl+R / Ctrl+Shift+R", "record");
    auto *recordMenu = new QMenu(record);
    recordMenu->addAction(tr("录制 GIF"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                          [this] { emit actionRequested(CaptureAction::RecordGif); });
    recordMenu->addAction(tr("录制 MP4"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this,
                          [this] { emit actionRequested(CaptureAction::RecordMp4); });
    record->setMenu(recordMenu);
    record->setPopupMode(QToolButton::InstantPopup);
    record->setStyleSheet("QToolButton::menu-indicator { image: none; }");
    QToolButton *beautify = addButton(outputRow, tr("美化：背景、圆角、阴影"), "Ctrl+B", "beautify");
    connect(beautify, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Beautify); });
    QToolButton *save = addButton(outputRow, tr("保存（Ctrl+Shift+S：不询问，直接存入保存文件夹）"),
                                  "Ctrl+S", "save");
    connect(save, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Save); });
    QToolButton *pin = addButton(outputRow, tr("贴到桌面"), "Ctrl+T", "pin");
    connect(pin, &QToolButton::clicked, this, [this] { emit actionRequested(CaptureAction::Pin); });
    m_done = addButton(outputRow, tr("复制到剪贴板"), "Ctrl+C", "done");
    m_done->setObjectName("done");
    connect(m_done, &QToolButton::clicked, this,
            [this] { emit actionRequested(m_pinEditing ? CaptureAction::Pin : CaptureAction::Copy); });
    m_captureOnly = {ocr, longShot, record, beautify, save, pin};

    setUndoState(false, false);
}

QToolButton *CaptureToolbar::addButton(QLayout *layout, const QString &text, const QString &shortcut,
                                       const char *icon)
{
    auto *button = new ToolbarButton(layout->parentWidget());
    button->setText(text);
    button->setToolTip(shortcut.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, shortcut));
    if (icon) {
        button->setIcon(toolIcon(icon));
        button->setIconSize(QSize(20, 20));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    } else {
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    layout->addWidget(button);
    return button;
}

QWidget *CaptureToolbar::buildOptions()
{
    auto *options = new QWidget(this);
    options->setObjectName("toolOptions");
    options->setAttribute(Qt::WA_StyledBackground);
    auto *layout = new ToolbarRow(2, options);
    layout->setContentsMargins(0, 4, 0, 0);

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
    auto *colors = new QButtonGroup(options);
    for (const QColor &color : Config::annotationPalette()) {
        auto *swatch = new ColorSwatch(color, options);
        swatch->setCheckable(true);
        swatch->setChecked(color == m_color);
        colors->addButton(swatch);
        m_swatches.append({color, swatch});
        layout->addWidget(swatch);
        connect(swatch, &QToolButton::clicked, this, [this, color] {
            m_color = color;
            Config::setAnnotationColor(color);
            emit styleChanged();
        });
    }
    m_fillButton = addButton(layout, tr("填充：实心矩形 / 椭圆，文字加底色"), QString(), "fill");
    m_fillButton->setIconSize(QSize(16, 16));
    m_fillButton->setCheckable(true);
    m_fillButton->setChecked(m_fill);
    connect(m_fillButton, &QToolButton::toggled, this, [this](bool on) {
        m_fill = on;
        Config::setAnnotationFill(on);
        emit styleChanged();
    });
    return options;
}

QSize CaptureToolbar::sizeForWidth(int maximumWidth) const
{
    ensurePolished();
    const int width = qMax(minimumSizeHint().width(), qMin(maximumWidth, sizeHint().width()));
    return QSize(width, heightForWidth(width));
}

void CaptureToolbar::setTool(Tool tool)
{
    m_tool = tool;
    for (const auto &[buttonTool, button] : std::as_const(m_toolButtons))
        button->setChecked(buttonTool == tool);
    updateOptionsVisibility();
}

void CaptureToolbar::setPinEditing(bool pinEditing)
{
    m_pinEditing = pinEditing;
    for (QToolButton *button : std::as_const(m_captureOnly))
        button->setVisible(!pinEditing);
    m_done->setText(pinEditing ? tr("完成标注") : tr("复制到剪贴板"));
    m_done->setToolTip(pinEditing ? tr("完成标注 (Enter)") : tr("复制到剪贴板 (Ctrl+C)"));
}

void CaptureToolbar::setSelectedAnnotationHasStyle(std::optional<bool> hasStyle)
{
    m_selectedAnnotationHasStyle = hasStyle;
    updateOptionsVisibility();
}

void CaptureToolbar::updateOptionsVisibility()
{
    m_options->setVisible(m_selectedAnnotationHasStyle.value_or(m_tool != Tool::None && m_tool != Tool::Spotlight));
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
