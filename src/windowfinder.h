#pragma once

#include <QList>
#include <QRect>
#include <QString>

// A visible top-level window and its visible descendants, in global physical pixels.
struct WindowNode
{
    QRect rect;
    QList<QRect> children;
};

// Visible top-level windows of other processes, topmost first.
QList<WindowNode> snapshotWindows();

// The foreground window's visible frame (global physical pixels); empty if it is the
// desktop, the taskbar or one of ours.
QRect foregroundWindowRect();

// Whether another application fills its monitor, including the taskbar area.
bool foregroundAppIsFullscreen();

// Smallest region under pos within the topmost window containing it; empty if none does.
QRect smallestRegionAt(const QList<WindowNode> &windows, const QPoint &pos);

struct CaptureWindow
{
    quintptr handle = 0;
    quint32 processId = 0;
    quint32 threadId = 0;
    QString title;
};

enum class CaptureWindowState { Available, Minimized, Unavailable, Closed };

// The chooser only shows visible, titled external windows. A selected window's
// identity is checked again while capturing because Windows can recycle HWNDs.
QList<CaptureWindow> snapshotCaptureWindows();
CaptureWindowState captureWindowState(const CaptureWindow &window);
