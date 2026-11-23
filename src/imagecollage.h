#pragma once

#include <QImage>
#include <QList>
#include <QString>

namespace ImageCollage {

enum class Direction { Horizontal, Vertical };

inline constexpr int kMaxDimension = 32768;
inline constexpr qint64 kMaxPixels = 64'000'000;

// The output uses source pixel sizes, a top/left alignment and transparent gaps.
QSize outputSize(const QList<QImage> &images, Direction direction, int spacing,
                 QString *error = nullptr);
QImage compose(const QList<QImage> &images, Direction direction, int spacing,
               QString *error = nullptr);
QImage preview(const QList<QImage> &images, Direction direction, int spacing,
               const QSize &bounds);
bool writePng(const QImage &image, const QString &path, QString *error = nullptr);

} // namespace ImageCollage
