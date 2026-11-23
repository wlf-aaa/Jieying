#pragma once

#include <QImage>
#include <QList>
#include <QPixmap>
#include <QString>

class QMimeData;
class QWidget;

namespace ImageIO {

void copyToClipboard(const QPixmap &image);

// Saves following the settings: asks where (starting in the save folder, with the template
// name and default format) or writes straight into the save folder; quick never asks.
// Returns the saved path, or an empty string if cancelled or failed.
QString saveImage(const QPixmap &image, QWidget *parent = nullptr, bool quick = false);

// Always asks for an export path, using the configured folder, name and format.
QString saveImageAs(const QPixmap &image, QWidget *parent = nullptr);

// Same for an already written file (a recording): moves it to the chosen place. Returns the
// new path, or an empty string if cancelled or failed; the source is left alone then.
QString saveRecording(const QString &sourcePath, const QString &suffix, QWidget *parent = nullptr);

// What pinning the clipboard (or a drop) shows: the image itself; else the image files among
// copied/dragged files; else the text rendered as an image (a color code as a swatch).
// Rendered text uses dpr device pixels per logical pixel.
QList<QImage> pinnableImages(const QMimeData *data, qreal dpr);

// Text as a pin: dark text on white with a margin, long lines wrapped.
QImage renderText(const QString &text, qreal dpr);

// Path in folder for a new file named after the template, made unique with "_2", "_3", ...
QString uniquePath(const QString &folder, const QString &suffix);

} // namespace ImageIO
