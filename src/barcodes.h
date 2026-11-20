#pragma once

#include <QImage>
#include <QList>
#include <QPolygon>
#include <QString>

// A QR code or barcode found in an image.
struct Barcode
{
    QString text;
    QString format;   // e.g. "QRCode", "EAN-13"
    QPolygon corners; // in image pixels: top-left, top-right, bottom-right, bottom-left
};

namespace Barcodes {

// All QR codes and barcodes in the image (ZXing-cpp, any rotation, dark-on-light or inverted).
// Safe to call from a worker thread.
QList<Barcode> detect(const QImage &image);

// A QR code image for the text (module size pixels per cell, with quiet zone). Used by tests.
QImage makeQrCode(const QString &text, int moduleSize);

} // namespace Barcodes
