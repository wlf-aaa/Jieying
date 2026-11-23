#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QMenu;
class QToolButton;

// Borderless always-on-top window showing a pinned image.
// Drag to move, wheel to zoom, Ctrl+wheel for opacity, double-click, Esc or Delete to close;
// R / Shift+R rotate, H / V flip, X crops, P or F4 lets clicks pass through (undone from the
// tray menu).
// Image files dropped on it are pinned beside it.
class PinWindow : public QWidget
{
    Q_OBJECT

public:
    // contentTopLeft is where the image's top-left lands, in global logical coordinates.
    PinWindow(const QPixmap &image, const QPoint &contentTopLeft, qreal initialScale = 1.0);

    QPixmap image() const { return m_image; }

    // Back to 100% zoom and full opacity, so the image shows its own pixels; returns where the
    // image is now (global logical coordinates).
    QRect prepareForEditing();
    // Shows an edited image in place of the current one, top-left kept.
    void replaceImage(const QPixmap &image);

    // Turns (or flips) the image; the pin keeps its center.
    void rotate(int degrees);
    void flip(bool horizontal);

    // Crop mode: drag the part to keep, Enter applies, Esc leaves the image as it was.
    void beginCrop();
    // Keeps the part of the image shown at contentRect (window-local logical pixels, inside the
    // image); that part stays where it is on screen.
    void cropTo(const QRect &contentRect);

    // Clicks and the wheel go to whatever is under the pin; it can no longer be focused either.
    void setClickThrough(bool on);
    bool clickThrough() const { return m_clickThrough; }

    int group() const { return m_group; } // see PinBoard
    void setGroup(int group) { m_group = group; }

signals:
    void ocrRequested(const QPixmap &image);
    void annotateRequested();
    void beautifyRequested(const QPixmap &image);
    void clickThroughChanged(bool on);
    void closing();
    void imagesDropped(const QList<QImage> &images, const QPoint &globalPos);
    // The context menu is about to open; receivers may add entries (directly connected).
    void menuAboutToShow(QMenu *menu);

protected:
    void closeEvent(QCloseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    friend class PinBoard;

    struct Snapshot
    {
        QPixmap image;
        QPoint contentTopLeft;
        int group = 1;
        qreal scale = 1.0;
        qreal opacity = 1.0;
        bool positionLocked = false;
    };

    explicit PinWindow(const Snapshot &state);
    Snapshot snapshot() const;
    QRect contentGeometry() const;
    QSize contentSize() const;
    void setScale(qreal scale, const QPointF &anchorGlobal);
    void setOpacity(qreal opacity);
    void setPositionLocked(bool locked);
    void showHint(const QString &text);
    void copy();
    void save();
    void setImageKeepingCenter(const QPixmap &image);
    void createQuickBar();
    QRect quickBarTrigger() const;
    void maybeShowQuickBar(const QPoint &pointer);
    void placeQuickBar(const QPoint &pointer);
    void checkQuickBarHover();
    void hideQuickBar();
    void syncQuickOpacity();
    void populateOpacityMenu(QMenu *menu);
    void trackPinMenu(QMenu *menu);
    void showPinMenu(const QPoint &globalPos);

    QPixmap m_image;
    qreal m_scale = 1.0;
    bool m_positionLocked = false;
    bool m_dragging = false;
    QPoint m_dragOffset;
    QString m_hint;
    QTimer m_hintTimer;
    bool m_clickThrough = false;
    int m_group = 1;
    bool m_cropping = false;
    QPoint m_cropStart;
    QRect m_cropRect; // window-local, normalized; empty until dragged
    QWidget *m_quickBar = nullptr;
    QToolButton *m_quickLock = nullptr;
    QToolButton *m_quickOpacity = nullptr;
    QPointer<QMenu> m_openMenu;
    QTimer m_quickHoverTimer;
    QElapsedTimer m_pointerAway;
};
