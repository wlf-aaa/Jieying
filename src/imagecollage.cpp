#include "imagecollage.h"

#include <QCoreApplication>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>

#include <algorithm>

namespace ImageCollage {

namespace {

QString tr(const char *message)
{
    return QCoreApplication::translate("ImageCollage", message);
}

QSize invalidSize(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return {};
}

QImage render(const QList<QImage> &images, Direction direction, int spacing,
              const QSize &fullSize, const QSize &renderSize)
{
    QImage canvas(renderSize, QImage::Format_ARGB32);
    if (canvas.isNull())
        return {};

    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    if (renderSize != fullSize) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(qreal(renderSize.width()) / fullSize.width(),
                      qreal(renderSize.height()) / fullSize.height());
    }

    int offset = 0;
    const bool horizontal = direction == Direction::Horizontal;
    for (const QImage &image : images) {
        if (offset)
            offset += spacing;
        const QRectF source(image.rect());
        const QPointF topLeft = horizontal ? QPointF(offset, 0) : QPointF(0, offset);
        painter.drawImage(QRectF(topLeft, source.size()), image, source);
        offset += horizontal ? image.width() : image.height();
    }
    painter.end();
    return canvas;
}

} // namespace

QSize outputSize(const QList<QImage> &images, Direction direction, int spacing, QString *error)
{
    if (error)
        error->clear();
    if (images.isEmpty())
        return invalidSize(error, tr("请先添加图片。"));
    if (spacing < 0)
        return invalidSize(error, tr("间距必须大于或等于 0。"));

    const bool horizontal = direction == Direction::Horizontal;
    qint64 length = 0;
    int breadth = 0;
    for (const QImage &image : images) {
        if (image.isNull())
            return invalidSize(error, tr("拼图中有无法读取的图片。"));
        if (length)
            length += spacing;
        length += horizontal ? image.width() : image.height();
        breadth = std::max(breadth, horizontal ? image.height() : image.width());
        if (length > kMaxDimension || breadth > kMaxDimension || length * breadth > kMaxPixels) {
            return invalidSize(error, tr("拼图尺寸过大。单边最多 %1 像素，总计最多 %2 万像素。")
                                          .arg(kMaxDimension).arg(kMaxPixels / 10'000));
        }
    }
    return horizontal ? QSize(int(length), breadth) : QSize(breadth, int(length));
}

QImage compose(const QList<QImage> &images, Direction direction, int spacing, QString *error)
{
    const QSize size = outputSize(images, direction, spacing, error);
    if (size.isEmpty())
        return {};
    const QImage result = render(images, direction, spacing, size, size);
    if (result.isNull() && error)
        *error = tr("内存不足，无法生成拼图。");
    return result;
}

QImage preview(const QList<QImage> &images, Direction direction, int spacing, const QSize &bounds)
{
    const QSize fullSize = outputSize(images, direction, spacing);
    if (fullSize.isEmpty() || bounds.isEmpty())
        return {};

    const QSize fit = QSizeF(fullSize).scaled(bounds, Qt::KeepAspectRatio).toSize();
    const QSize renderSize = fit.expandedTo(QSize(1, 1)).boundedTo(fullSize);
    return render(images, direction, spacing, fullSize, renderSize);
}

bool writePng(const QImage &image, const QString &path, QString *error)
{
    if (error)
        error->clear();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QImageWriter writer(&file, "png");
    if (!writer.write(image)) {
        if (error)
            *error = writer.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace ImageCollage
