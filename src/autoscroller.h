#pragma once

#include "scrollstitcher.h"

// Decides, tick by tick, whether automatic scrolling for a long screenshot goes on.
// Pure logic: the caller sends the wheel input and reports what the stitcher saw.
class AutoScroller
{
public:
    enum class Decision {
        Scroll,       // send the next wheel notch
        StopAtEnd,    // several notches changed nothing: reached the bottom
        StopByUser,   // the user moved the mouse: they take over
        StopLost,     // a notch scrolled past the region (too short to overlap)
        StopFull,     // maximum height reached
    };

    // Called before every notch with what happened since the previous one.
    Decision next(bool appended, bool lost, bool full, bool userMovedMouse);
    void reset() { m_idleTicks = 0; }

    static constexpr int kIdleTicksAtEnd = 3;

private:
    int m_idleTicks = 0;
};
