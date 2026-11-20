#pragma once

#include <QImage>
#include <QRect>

class QWidget;

// Copies a rectangle of the desktop (global physical pixels) via GDI, optionally with the cursor.
QImage grabScreenRect(const QRect &physicalRect, bool includeCursor);

// Keeps a window out of screenshots and recordings, ours included (Windows 10 2004+).
bool excludeFromCapture(QWidget *window);
