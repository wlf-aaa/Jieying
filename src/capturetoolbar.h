#pragma once

#include "annotations.h"

#include <QColor>
#include <QList>
#include <QWidget>

class QHBoxLayout;
class QToolButton;

// QuickSave: straight into the save folder, never asking where.
enum class CaptureAction { Copy, Save, Pin, Ocr, LongShot, RecordGif, RecordMp4, Beautify, QuickSave };

// Actions that hand the region (not an image) on to a follow-up capture mode.
inline bool isRegionAction(CaptureAction action)
{
    return action == CaptureAction::LongShot || action == CaptureAction::RecordGif
           || action == CaptureAction::RecordMp4;
}

// Toolbar order.
inline constexpr Tool kToolOrder[] = {Tool::Rect,   Tool::Ellipse,     Tool::Line, Tool::Arrow,  Tool::Ruler,
                                      Tool::Pen,    Tool::Highlighter, Tool::Text, Tool::Mosaic, Tool::Blur,
                                      Tool::Number, Tool::Eraser};
// Shift+1..9 selects annotation tools (index 0 is unused).
inline constexpr Tool kToolByShiftDigit[10] = {Tool::None,   Tool::Rect,        Tool::Line,   Tool::Arrow, Tool::Number,
                                               Tool::Pen,    Tool::Highlighter, Tool::Mosaic, Tool::Text,  Tool::Eraser};
// Ctrl+digit picks a tool. 1-7 kept their original tools when more were added.
inline constexpr Tool kToolByDigit[10] = {Tool::Blur, Tool::Rect,   Tool::Ellipse, Tool::Arrow, Tool::Pen,
                                          Tool::Text, Tool::Mosaic, Tool::Number,  Tool::Line,  Tool::Highlighter};

// Toolbar under the selection: annotation tools, their color/size options,
// undo/redo, and the actions that finish the capture.
class CaptureToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit CaptureToolbar(QWidget *parent = nullptr);

    Tool tool() const { return m_tool; }
    void setTool(Tool tool);
    QColor color() const { return m_color; }
    int sizeLevel() const { return m_sizeLevel; } // 0 small, 1 medium, 2 large
    bool fill() const { return m_fill; }
    void setUndoState(bool canUndo, bool canRedo);

    // Annotating a pinned image: only the drawing tools, undo/redo, cancel and a "done" check
    // (which then requests CaptureAction::Pin, i.e. "put it back on the pin").
    void setPinEditing(bool pinEditing);

    // Keeps the color/size row visible without an active tool (an annotation is selected).
    void setOptionsVisible(bool visible);
    // Shows a selected annotation's color and size; not saved as the default.
    void showStyle(const QColor &color, int sizeLevel, bool fill);

signals:
    void toolChanged(Tool tool);
    void styleChanged();
    void redactRequested();
    void undoRequested();
    void redoRequested();
    void cancelRequested();
    void actionRequested(CaptureAction action);

private:
    // With an icon (assets/icons/<icon>.svg) the button shows only the icon; text and shortcut
    // go into the tooltip.
    QToolButton *addButton(QHBoxLayout *layout, const QString &text, const QString &shortcut,
                           const char *icon = nullptr);
    void addSeparator(QHBoxLayout *layout);
    QWidget *buildOptions();

    Tool m_tool = Tool::None;
    QColor m_color;
    int m_sizeLevel = 1;
    bool m_fill = false;
    QToolButton *m_fillButton = nullptr;
    QList<std::pair<Tool, QToolButton *>> m_toolButtons;
    QToolButton *m_undo = nullptr;
    QToolButton *m_redo = nullptr;
    QWidget *m_options = nullptr;
    bool m_optionsForced = false;
    bool m_pinEditing = false;
    QToolButton *m_done = nullptr;
    QList<QToolButton *> m_captureOnly; // hidden while editing a pin
    QList<QToolButton *> m_sizeButtons;
    QList<std::pair<QColor, QToolButton *>> m_swatches;
};
