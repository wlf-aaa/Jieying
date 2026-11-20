#pragma once

#include <QList>
#include <QRect>

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

// Smallest region under pos within the topmost window containing it; empty if none does.
QRect smallestRegionAt(const QList<WindowNode> &windows, const QPoint &pos);
