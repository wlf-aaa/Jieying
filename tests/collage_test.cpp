#include "imagecollage.h"

#include <QColor>
#include <QCoreApplication>
#include <QImageReader>
#include <QTemporaryDir>

#include <iostream>
#include <limits>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

QImage imageOfSize(int width, int height, const QColor &color)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    using ImageCollage::Direction;

    QImage red = imageOfSize(3, 2, Qt::red);
    red.setDevicePixelRatio(2);
    const QColor translucentBlue(0, 0, 255, 128);
    const QImage blue = imageOfSize(2, 4, translucentBlue);

    const QImage horizontal = ImageCollage::compose({red, blue}, Direction::Horizontal, 2);
    check(horizontal.size() == QSize(7, 4), "horizontal size uses source pixels and one gap");
    if (horizontal.size() == QSize(7, 4)) {
        check(horizontal.pixelColor(2, 1) == Qt::red, "high-DPI source keeps all its pixels");
        check(horizontal.pixelColor(6, 3) == translucentBlue, "later image keeps its alpha");
        check(horizontal.pixelColor(4, 0).alpha() == 0, "spacing has a transparent background");
        check(horizontal.pixelColor(0, 3).alpha() == 0, "uneven heights are top-aligned");
    }

    const QImage vertical = ImageCollage::compose({blue, red}, Direction::Vertical, 1);
    check(vertical.size() == QSize(3, 7), "vertical size uses source pixels and one gap");
    if (vertical.size() == QSize(3, 7)) {
        check(vertical.pixelColor(0, 0) == translucentBlue, "vertical layout keeps input order");
        check(vertical.pixelColor(2, 6) == Qt::red, "vertical layout appends along the height");
        check(vertical.pixelColor(0, 4).alpha() == 0, "vertical spacing is transparent");
        check(vertical.pixelColor(2, 3).alpha() == 0, "uneven widths are left-aligned");
    }

    const QImage one = ImageCollage::compose({red}, Direction::Vertical, 12);
    check(one.size() == QSize(3, 2), "a single image gets no outer spacing");
    const QImage adjacent = ImageCollage::compose({red, blue}, Direction::Horizontal, 0);
    check(adjacent.size() == QSize(5, 4), "zero spacing makes images adjacent");

    QString error;
    check(ImageCollage::compose({}, Direction::Horizontal, 0, &error).isNull() && !error.isEmpty(),
          "empty input is reported");
    check(ImageCollage::compose({red, QImage()}, Direction::Vertical, 0).isNull(),
          "an unreadable image is rejected");
    check(ImageCollage::compose({red}, Direction::Horizontal, -1).isNull(),
          "negative spacing is rejected");
    check(ImageCollage::compose({red, red}, Direction::Horizontal, std::numeric_limits<int>::max()).isNull(),
          "dimension overflow is rejected before allocation");
    const QImage wide = imageOfSize(16000, 1, Qt::black);
    const QImage tall = imageOfSize(1, 16000, Qt::black);
    check(ImageCollage::compose({wide, tall}, Direction::Horizontal, 0).isNull(),
          "too many output pixels are rejected even with small sources");

    const QImage preview = ImageCollage::preview({horizontal}, Direction::Horizontal, 0, QSize(3, 3));
    check(preview.size() == QSize(3, 2),
          "preview fits inside the available space");
    const QImage panoramicPreview = ImageCollage::preview({wide}, Direction::Horizontal, 0, QSize(160, 100));
    check(panoramicPreview.size() == QSize(160, 1), "extremely wide output still has a visible preview");

    const QTemporaryDir output;
    check(output.isValid(), "temporary output directory is available");
    if (output.isValid()) {
        const QString path = output.filePath(QStringLiteral("collage.preview"));
        check(ImageCollage::writePng(horizontal, path, &error), "collage can be saved");
        QImageReader reader(path);
        check(reader.format() == "png", "export always encodes PNG");
        const QImage written = reader.read();
        check(written == horizontal, "PNG round-trip preserves size, colors and transparency");
        check(!ImageCollage::writePng(horizontal, output.filePath(QStringLiteral("missing/out.png")), &error)
                  && !error.isEmpty(),
              "an unwritable destination is reported");
    }

    if (!failures)
        std::cout << "Collage tests passed\n";
    return failures ? 1 : 0;
}
