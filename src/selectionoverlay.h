#pragma once

#include "annotations.h"
#include "ocr.h"
#include "capturetoolbar.h"
#include "screengrab.h"
#include "windowfinder.h"

#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QFutureWatcher>
#include <QTimer>
#include <QWidget>

#include <optional>

class QPlainTextEdit;
class QScreen;

// Frozen full-screen snapshot of one monitor where the user picks and annotates a region.
// All geometry is kept in this screen's physical pixels and mapped to logical only for painting,
// so selections and annotations stay pixel-exact at fractional scale factors.
class SelectionOverlay : public QWidget
{
    Q_OBJECT

public:
    SelectionOverlay(QScreen *screen, const QPixmap &shot, const QList<WindowNode> &windows);

    // Another overlay owns the selection, so this one stops highlighting windows.
    void setPassive(bool passive);

    // Run this action as soon as a selection is made (hotkeys for long screenshot / recording).
    void setAutoAction(std::optional<CaptureAction> action);

    // Annotate a fixed region (a pinned image painted into the shot): it cannot be moved,
    // resized or replaced; done/Enter emits actionTriggered(Pin, ...) with the result and
    // Esc/right-click cancels.
    void editFixedRegion(const QRect &physicalRegion);

    // Starts with this region already selected (full screen / active window); it can still be
    // adjusted, annotated or replaced like a dragged selection.
    void preselect(const QRect &physicalRegion);
    ScreenshotArea captureArea() const;
    static CaptureAction configuredConfirmAction();

    // Freeform: the region is drawn by hand; outside the shape the result is transparent.
    // F toggles it while choosing a region.
    void setFreeform(bool on);

signals:
    void selectionStarted();
    void selectionCleared();
    void actionTriggered(CaptureAction action, const QPixmap &image, const QPoint &globalTopLeft);
    // Long screenshot / recording: region in this screen's local physical pixels.
    void regionActionTriggered(CaptureAction action, QScreen *screen, const QRect &region);
    // One-click redaction finished: count marks added, -1 when recognition failed.
    void redactFinished(int count);
    void cancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class State { Hover, Dragging, Selected, Moving, Resizing, Annotating, MovingAnnotation, Erasing, Lassoing };
    enum Edge { None = 0, Left = 1, Top = 2, Right = 4, Bottom = 8 };

    void buildToolbar();
    QPoint toPhysical(const QPointF &logical) const;
    QRectF toLogical(const QRect &physical) const;
    QRect regionAt(const QPoint &pos) const;
    QRect currentRegion() const;
    int edgesAt(const QPointF &pos) const;
    int spotlightHandleAt(const QPointF &pos) const;
    static Qt::CursorShape resizeCursor(int edges);
    void moveSelectionTo(QRect rect);
    void resetToHover();
    void updateCursor(const QPointF &pos);
    void updateToolbar();
    void trigger(CaptureAction action);

    AnnotationStyle currentStyle() const;
    void onToolChanged();
    void updateDraft(Qt::KeyboardModifiers modifiers);
    void finishDraft();
    void updateAnnotationDrag();
    // Removes every mark the eraser touches on its way from -> to (a fast flick skips pixels).
    void eraseAlong(const QPoint &from, const QPoint &to);
    // Recognizes the selection's text in the background and pixelates personal data in it.
    void redactSensitive();
    void showNotice(const QString &text, int ms = 3000);
    void undo();
    void redo();
    void refreshUndoState();
    // Opens the inline editor at pos; with text/style it re-edits an existing text mark.
    void beginText(const QPoint &pos, const QString &text = QString(),
                   std::optional<AnnotationStyle> style = std::nullopt);
    qreal hitTolerance() const;
    void selectAnnotation(int index);
    void editTextAnnotation(int index);
    void applyTextStyle();
    void layoutTextEdit();
    void commitText();

    void drawHandles(QPainter &p, const QRectF &r) const;
    void drawSizeLabel(QPainter &p, const QRect &region) const;
    void drawMagnifier(QPainter &p) const;
    QString colorText(const QColor &color) const;
    bool picking() const; // the magnifier is up: hovering, dragging or resizing
    void selectTool(Tool tool); // toggles: picking the active tool again turns it off
    void drawAutoActionBanner(QPainter &p) const;
    void drawBanner(QPainter &p, const QString &text) const;
    CaptureAction confirmAction() const;

    QScreen *m_screen;
    std::optional<CaptureAction> m_autoAction;
    bool m_showMagnifier;
    bool m_rgbColors = false; // magnifier shows (and C copies) "R, G, B" instead of #RRGGBB
    CaptureAction m_confirmAction;
    qreal m_dpr;
    QPixmap m_shot;
    QImage m_image;               // same pixels as m_shot, for sampling and composing
    QList<WindowNode> m_windows;  // screen-local physical pixels

    State m_state = State::Hover;
    bool m_passive = false;
    bool m_fixedRegion = false;
    bool m_cursorInside = false;
    bool m_dragMoved = false;
    int m_resizeEdges = None;
    QRect m_selection;
    bool m_freeform = false;
    QPolygonF m_lassoPoints;  // being drawn
    QPainterPath m_lasso;     // the finished shape (physical pixels); empty: rectangle
    QRect m_hover;
    QPoint m_cursor;
    QPoint m_pressPos;
    QRect m_pressSelection;
    CaptureToolbar *m_toolbar = nullptr;

    QFutureWatcher<OcrResult> *m_redactWatcher = nullptr;
    QRect m_redactRegion; // where the running recognition's image came from
    QString m_notice;
    QTimer m_noticeTimer;

    AnnotationStack m_annotations;
    AnnotationPtr m_draft;        // annotation being drawn, not yet on the stack
    int m_selectedAnnotation = -1; // index into m_annotations, -1 for none
    AnnotationPtr m_movingOriginal;
    QPolygonF m_strokePoints;
    QPlainTextEdit *m_textEdit = nullptr;
    QPoint m_textPos;
    AnnotationStyle m_textStyle;
};
