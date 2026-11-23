#pragma once

#include <QImage>
#include <QPainterPath>
#include <QRect>

class AnnotationStack;
class QWidget;

// Bounds and optional freeform path in a single display's local physical pixels.
struct ScreenshotArea
{
    QRect bounds;
    QPainterPath mask;

    QImage render(const QImage &screen, const AnnotationStack *annotations = nullptr) const;
};

// Copies a rectangle of the desktop (global physical pixels) via GDI, optionally with the cursor.
QImage grabScreenRect(const QRect &physicalRect, bool includeCursor);

// Keeps a window out of screenshots and recordings, ours included (Windows 10 2004+).
bool excludeFromCapture(QWidget *window);
