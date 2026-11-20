#pragma once

#include <QPixmap>
#include <QTimer>
#include <QWidget>

class QMenu;

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
    PinWindow(const QPixmap &image, const QPoint &contentTopLeft);

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
    void imagesDropped(const QList<QImage> &images, const QPoint &globalPos);
    // The context menu is about to open; receivers may add entries (directly connected).
    void menuAboutToShow(QMenu *menu);

protected:
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
    QSize contentSize() const;
    void setScale(qreal scale, const QPointF &anchorGlobal);
    void setOpacity(qreal opacity);
    void showHint(const QString &text);
    void save();
    void setImageKeepingCenter(const QPixmap &image);

    QPixmap m_image;
    qreal m_scale = 1.0;
    bool m_dragging = false;
    QPoint m_dragOffset;
    QString m_hint;
    QTimer m_hintTimer;
    bool m_clickThrough = false;
    int m_group = 1;
    bool m_cropping = false;
    QPoint m_cropStart;
    QRect m_cropRect; // window-local, normalized; empty until dragged
};
