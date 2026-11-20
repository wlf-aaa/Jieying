#pragma once

#include "autoscroller.h"
#include "capturetoolbar.h"
#include "scrollstitcher.h"

#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QTimer>

class QLabel;
class QPushButton;
class QScreen;
class RegionFrame;

// Long screenshot: samples a screen region while the user scrolls its content (down or right)
// and stitches the frames, with a live preview panel beside the region.
class ScrollCapture : public QObject
{
    Q_OBJECT

public:
    // physicalRegion is in the screen's local physical pixels, as the overlay reports it.
    ScrollCapture(QScreen *screen, const QRect &physicalRegion, QObject *parent = nullptr);
    ~ScrollCapture() override;

    void start();

    // Longest result for a region width: a pixel budget, so a 4K-wide capture stops earlier
    // than a narrow one instead of using gigabytes.
    static int maxHeightFor(int width);
    void finish(CaptureAction action);
    void cancel();

signals:
    void finished(CaptureAction action, const QPixmap &image, const QPoint &globalTopLeft);
    void cancelled();

private:
    void sample();
    void buildPanel();
    // Automatic scrolling: real wheel notches over the region, as browsers and Office ignore
    // synthesized wheel messages. Started only by the user's click on the panel button.
    void startAutoScroll();
    void autoScrollTick();
    void stopAutoScroll(const QString &reason);
    void closeWindows();

    QScreen *m_screen;
    qreal m_dpr;
    QRect m_physical; // global physical pixels
    QRect m_logical;  // global logical pixels
    TwoWayStitcher m_stitcher;
    QTimer m_timer;
    QPointer<RegionFrame> m_frame;
    QPointer<QWidget> m_panel;
    QLabel *m_preview = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_autoButton = nullptr;
    QTimer m_autoTimer;
    AutoScroller m_autoScroller;
    QPoint m_autoCursor; // where we put the cursor; anything else means the user moved it
    bool m_appendedSinceTick = false;
    bool m_lostSinceTick = false;
    bool m_full = false;
};
