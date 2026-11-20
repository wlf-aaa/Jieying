#include "beautify.h"

#include <QCoreApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>

namespace Beautify {

namespace {

struct Background
{
    const char *name;
    QColor from, to; // invalid: transparent
};

const Background kBackgrounds[] = {
    {QT_TRANSLATE_NOOP("Beautify", "透明"), {}, {}},
    {QT_TRANSLATE_NOOP("Beautify", "海蓝渐变"), QColor("#4facfe"), QColor("#00f2fe")},
    {QT_TRANSLATE_NOOP("Beautify", "日落渐变"), QColor("#fa709a"), QColor("#fee140")},
    {QT_TRANSLATE_NOOP("Beautify", "薄荷渐变"), QColor("#43e97b"), QColor("#38f9d7")},
    {QT_TRANSLATE_NOOP("Beautify", "紫罗兰渐变"), QColor("#a18cd1"), QColor("#fbc2eb")},
    {QT_TRANSLATE_NOOP("Beautify", "深灰渐变"), QColor("#232526"), QColor("#414345")},
    {QT_TRANSLATE_NOOP("Beautify", "白色"), QColor(Qt::white), QColor(Qt::white)},
};
constexpr int kBackgroundCount = int(std::size(kBackgrounds));

// Soft blur by box-filtered downscale and smooth upscale: plenty for a drop shadow.
QImage blurred(const QImage &image, int radius)
{
    const int factor = qMax(2, radius / 2);
    const QImage small = image.scaled(qMax(1, image.width() / factor), qMax(1, image.height() / factor),
                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return small.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace

QStringList backgroundNames()
{
    QStringList names;
    for (const Background &background : kBackgrounds)
        names << QCoreApplication::translate("Beautify", background.name);
    return names;
}

Options loadOptions()
{
    QSettings settings;
    Options options;
    options.background = qBound(0, settings.value("beautify/background", options.background).toInt(), kBackgroundCount - 1);
    options.padding = qBound(0, settings.value("beautify/padding", options.padding).toInt(), 200);
    options.radius = qBound(0, settings.value("beautify/radius", options.radius).toInt(), 60);
    options.shadow = settings.value("beautify/shadow", options.shadow).toBool();
    return options;
}

void saveOptions(const Options &options)
{
    QSettings settings;
    settings.setValue("beautify/background", options.background);
    settings.setValue("beautify/padding", options.padding);
    settings.setValue("beautify/radius", options.radius);
    settings.setValue("beautify/shadow", options.shadow);
}

QImage render(const QImage &source, const Options &options)
{
    const qreal dpr = source.devicePixelRatio();
    const int padding = qRound(options.padding * dpr);
    const int radius = qRound(options.radius * dpr);
    QImage out(source.size() + QSize(2 * padding, 2 * padding), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);

    const Background &background = kBackgrounds[qBound(0, options.background, kBackgroundCount - 1)];
    const QRectF content(padding, padding, source.width(), source.height());
    QPainterPath shape;
    shape.addRoundedRect(content, radius, radius);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (background.from.isValid()) {
        QLinearGradient gradient(0, 0, out.width(), out.height());
        gradient.setColorAt(0, background.from);
        gradient.setColorAt(1, background.to);
        p.fillRect(out.rect(), gradient);
    }
    if (options.shadow && padding > 0) {
        QImage shadow(out.size(), QImage::Format_ARGB32_Premultiplied);
        shadow.fill(Qt::transparent);
        {
            QPainter s(&shadow);
            s.setRenderHint(QPainter::Antialiasing);
            s.fillPath(shape.translated(0, 8 * dpr), QColor(0, 0, 0, 110));
        }
        p.drawImage(0, 0, blurred(shadow, qRound(24 * dpr)));
    }
    p.setClipPath(shape);
    QImage pixels = source;
    pixels.setDevicePixelRatio(1);
    p.drawImage(content.topLeft(), pixels);
    p.end();

    out.setDevicePixelRatio(dpr);
    return out;
}

} // namespace Beautify
