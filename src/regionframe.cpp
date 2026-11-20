#include "regionframe.h"

#include "screengrab.h"

#include <QPainter>
#include <QScreen>
#include <QWindow>

namespace {
constexpr int kMargin = 3;
constexpr int kGap = 8;
} // namespace

const char *const kPanelStyle =
    "#panel { background: rgba(255,255,255,245); border: 1px solid rgba(0,0,0,60); border-radius: 6px; }"
    "#panel QPushButton { border: none; border-radius: 3px; padding: 5px 10px; color: #222; background: transparent; }"
    "#panel QPushButton:hover { background: rgba(30,144,255,45); }"
    "#panel QPushButton:checked { background: rgba(30,144,255,90); }"
    "#panel QPushButton:disabled { color: #aaa; }"
    "#panel QLabel { color: #333; }";

RegionFrame::RegionFrame(QScreen *screen, const QRect &logicalRegion, const QColor &color)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                           | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
    , m_color(color)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_DeleteOnClose);
    create();
    windowHandle()->setScreen(screen);
    setGeometry(logicalRegion.adjusted(-kMargin, -kMargin, kMargin, kMargin));
    excludeFromCapture(this);
}

void RegionFrame::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setPen(QPen(m_color, 2));
    p.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
}

QPoint placeBeside(QScreen *screen, const QRect &region, const QSize &size)
{
    const QRect area = screen->availableGeometry();
    const QPoint candidates[] = {
        {region.right() + kGap + kMargin, region.top()},
        {region.left() - kGap - kMargin - size.width(), region.top()},
        {region.right() - size.width() + 1, region.bottom() + kGap + kMargin},
        {region.right() - size.width() + 1, region.top() - kGap - kMargin - size.height()},
    };
    for (const QPoint &pos : candidates) {
        if (area.contains(QRect(pos, size)))
            return pos;
    }
    const QPoint inside(region.right() - size.width() - kGap, region.top() + kGap);
    return QPoint(qBound(area.left(), inside.x(), area.right() - size.width()),
                  qBound(area.top(), inside.y(), qMax(area.top(), area.bottom() - size.height())));
}
