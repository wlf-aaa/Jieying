#include "imageio.h"

#include "config.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QUrl>

namespace ImageIO {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ImageIO", text);
}

void warnWriteFailed(QWidget *parent, const QString &path)
{
    QMessageBox::warning(parent, tr("保存失败"), tr("无法写入文件：\n%1").arg(QDir::toNativeSeparators(path)));
}

// Asks for a target path, starting in the save folder with the template name.
QString askPath(QWidget *parent, const QString &title, const QString &suffix, const QString &filter)
{
    const QString start = QDir(Config::effectiveSaveDir())
                              .filePath(Config::expandFileName(Config::fileNameTemplate(), suffix,
                                                               QDateTime::currentDateTime()));
    const QString path = QFileDialog::getSaveFileName(parent, title, start, filter);
    if (!path.isEmpty())
        Config::rememberSaveDir(QFileInfo(path).absolutePath());
    return path;
}

// Where to save without asking; empty if the folder cannot be created.
QString directPath(QWidget *parent, const QString &suffix)
{
    const QString folder = Config::effectiveSaveDir();
    if (!QDir().mkpath(folder)) {
        warnWriteFailed(parent, folder);
        return {};
    }
    return uniquePath(folder, suffix);
}

bool writeImage(const QPixmap &image, const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool lossy = suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") || suffix == QLatin1String("webp");
    return image.save(path, nullptr, lossy ? Config::jpgQuality() : -1);
}

} // namespace

QList<QImage> pinnableImages(const QMimeData *data, qreal dpr)
{
    if (!data)
        return {};
    if (data->hasImage()) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        if (!image.isNull())
            return {image};
    }
    if (data->hasUrls()) {
        QList<QImage> images;
        for (const QUrl &url : data->urls()) {
            if (!url.isLocalFile())
                continue;
            QImageReader reader(url.toLocalFile());
            reader.setAutoTransform(true); // photos: honor the EXIF orientation
            const QImage image = reader.read();
            if (!image.isNull())
                images.append(image);
            if (images.size() == 20) // a whole folder by mistake: stop somewhere sensible
                break;
        }
        if (!images.isEmpty())
            return images;
    }
    const QString text = data->text().trimmed();
    if (text.isEmpty())
        return {};

    // A color code (#RRGGBB, #RGB or "r, g, b") becomes a swatch.
    static const QRegularExpression rgb(QStringLiteral(R"(^(?:rgb\s*\()?\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\s*\)?$)"),
                                        QRegularExpression::CaseInsensitiveOption);
    QColor color;
    if (QColor::isValidColorName(text) && text.startsWith(QLatin1Char('#'))) {
        color = QColor::fromString(text);
    } else if (const auto m = rgb.match(text); m.hasMatch() && m.captured(1).toInt() < 256
                                                && m.captured(2).toInt() < 256 && m.captured(3).toInt() < 256) {
        color = QColor(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    }
    if (color.isValid()) {
        QImage swatch(QSize(160, 80) * dpr, QImage::Format_RGB32);
        swatch.fill(color);
        QPainter p(&swatch);
        QFont font = QGuiApplication::font();
        font.setPixelSize(qRound(13 * dpr));
        p.setFont(font);
        p.setPen(color.lightness() > 160 ? Qt::black : Qt::white);
        p.drawText(swatch.rect(), Qt::AlignCenter,
                   QStringLiteral("%1\n%2, %3, %4")
                       .arg(color.name().toUpper())
                       .arg(color.red())
                       .arg(color.green())
                       .arg(color.blue()));
        return {swatch};
    }
    return {renderText(text, dpr)};
}

QImage renderText(const QString &text, qreal dpr)
{
    constexpr int kMargin = 12;
    constexpr int kMaxWidth = 600; // logical px before wrapping
    QFont font = QGuiApplication::font();
    font.setPixelSize(qRound(15 * dpr));
    const QFontMetrics fm(font);
    const QString shown = text.left(20'000);
    const int flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap | Qt::TextExpandTabs;
    const QRect bounds = fm.boundingRect(QRect(0, 0, qRound(kMaxWidth * dpr), 1 << 20), flags, shown);
    const int margin = qRound(kMargin * dpr);
    QImage image(qMin(bounds.height(), 16'000) > 0 ? bounds.size().grownBy({margin, margin, margin, margin})
                                                    : QSize(2 * margin, 2 * margin),
                 QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    p.setFont(font);
    p.setPen(QColor(30, 30, 30));
    p.drawText(QRect(margin, margin, bounds.width(), bounds.height()), flags, shown);
    return image;
}

void copyToClipboard(const QPixmap &image)
{
    QGuiApplication::clipboard()->setImage(image.toImage());
}

QString uniquePath(const QString &folder, const QString &suffix)
{
    const QDir dir(folder);
    const QString name = Config::expandFileName(Config::fileNameTemplate(), suffix, QDateTime::currentDateTime());
    QString path = dir.filePath(name);
    const QString base = QFileInfo(name).completeBaseName();
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = dir.filePath(QStringLiteral("%1_%2.%3").arg(base).arg(n).arg(suffix));
    return path;
}

QString saveImage(const QPixmap &image, QWidget *parent, bool quick)
{
    const QString format = Config::imageFormat();
    QString path;
    if (Config::askWhereToSave() && !quick) {
        QStringList filters = {tr("PNG 图片 (*.png)"), tr("JPEG 图片 (*.jpg *.jpeg)"), tr("WebP 图片 (*.webp)")};
        const int preferred = format == QLatin1String("jpg") ? 1 : format == QLatin1String("webp") ? 2 : 0;
        filters.move(preferred, 0); // the dialog starts on the first one
        const QString filter = filters.join(QStringLiteral(";;")) + tr(";;BMP 图片 (*.bmp)");
        path = askPath(parent, tr("保存截图"), format, filter);
    } else {
        path = directPath(parent, format);
    }
    if (path.isEmpty())
        return {};
    if (!writeImage(image, path)) {
        warnWriteFailed(parent, path);
        return {};
    }
    return path;
}

QString saveRecording(const QString &sourcePath, const QString &suffix, QWidget *parent)
{
    const QString filter = suffix == QLatin1String("gif") ? tr("GIF 动图 (*.gif)") : tr("MP4 视频 (*.mp4)");
    const QString path = Config::askWhereToSave() ? askPath(parent, tr("保存录屏"), suffix, filter)
                                                  : directPath(parent, suffix);
    if (path.isEmpty())
        return {};
    QFile::remove(path); // the dialog already confirmed overwriting; direct paths are new
    if (!QFile::rename(sourcePath, path)) {
        // rename fails across volumes (temp dir on C:, target on D:)
        if (!QFile::copy(sourcePath, path)) {
            warnWriteFailed(parent, path);
            return {};
        }
        QFile::remove(sourcePath);
    }
    return path;
}

} // namespace ImageIO
