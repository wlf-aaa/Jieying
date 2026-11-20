#pragma once

#include <QColor>
#include <QWidget>

class QScreen;

// Click-through border drawn just outside a capture region. It is excluded from screen
// capture, so it never shows up in long screenshots or recordings.
class RegionFrame : public QWidget
{
public:
    RegionFrame(QScreen *screen, const QRect &logicalRegion, const QColor &color);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor m_color;
};

// Top-left for a panel of the given size next to the region (right, left, below, above,
// else inside the top-right corner), within the screen's available area.
QPoint placeBeside(QScreen *screen, const QRect &logicalRegion, const QSize &panelSize);

// Common look for the small floating control panels.
extern const char *const kPanelStyle;
