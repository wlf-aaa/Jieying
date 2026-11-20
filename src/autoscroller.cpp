#include "autoscroller.h"

AutoScroller::Decision AutoScroller::next(bool appended, bool lost, bool full, bool userMovedMouse)
{
    if (userMovedMouse)
        return Decision::StopByUser;
    if (full)
        return Decision::StopFull;
    if (lost)
        return Decision::StopLost;
    if (appended) {
        m_idleTicks = 0;
        return Decision::Scroll;
    }
    // Nothing new: either the page is still animating or the end was reached.
    return ++m_idleTicks >= kIdleTicksAtEnd ? Decision::StopAtEnd : Decision::Scroll;
}
