#include "barcodes.h"

#include <CreateBarcode.h> // ZXing-cpp, built in-tree from third_party/
#include <ReadBarcode.h>
#include <WriteBarcode.h>

QList<Barcode> Barcodes::detect(const QImage &source)
{
    const QImage gray = source.convertToFormat(QImage::Format_Grayscale8);
    const ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(), ZXing::ImageFormat::Lum,
                                int(gray.bytesPerLine()));
    ZXing::ReaderOptions options;
    options.setTryHarder(true);
    options.setTryRotate(true);
    options.setTryInvert(true); // light-on-dark codes in dark themes
    options.setTryDownscale(true);

    QList<Barcode> found;
    for (const ZXing::Barcode &barcode : ZXing::ReadBarcodes(view, options)) {
        if (!barcode.isValid())
            continue;
        const ZXing::Position &p = barcode.position();
        Barcode result;
        result.text = QString::fromStdString(barcode.text());
        result.format = QString::fromStdString(ZXing::ToString(barcode.format()));
        result.corners << QPoint(p.topLeft().x, p.topLeft().y) << QPoint(p.topRight().x, p.topRight().y)
                       << QPoint(p.bottomRight().x, p.bottomRight().y) << QPoint(p.bottomLeft().x, p.bottomLeft().y);
        found.append(result);
    }
    return found;
}

QImage Barcodes::makeQrCode(const QString &text, int moduleSize)
{
    const ZXing::Barcode barcode =
        ZXing::CreateBarcodeFromText(text.toStdString(), ZXing::CreatorOptions(ZXing::BarcodeFormat::QRCode));
    const ZXing::Image image = ZXing::WriteBarcodeToImage(barcode, ZXing::WriterOptions().scale(moduleSize));
    return QImage(image.data(), image.width(), image.height(), image.rowStride(), QImage::Format_Grayscale8).copy();
}
