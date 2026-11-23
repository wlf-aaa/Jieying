#include "capturehistory.h"

#include "config.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QImageReader>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>

CaptureHistory::CaptureHistory(const QString &folder, QObject *parent)
    : QObject(parent)
    , m_folder(folder)
{
}

QString CaptureHistory::defaultFolder()
{
    if (QSettings::defaultFormat() == QSettings::IniFormat) // portable copy: keep everything beside the exe
        return QDir(QCoreApplication::applicationDirPath()).filePath("data/history");
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("history");
}

void CaptureHistory::add(const QImage &image)
{
    if (image.isNull() || Config::historySize() <= 0)
        return;
    QDir().mkpath(m_folder);
    // Time-stamped names sort chronologically; the suffix keeps two captures in one ms apart.
    const QString base = QDir(m_folder).filePath(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"));
    QString path = base + QStringLiteral(".png");
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QStringLiteral("%1_%2.png").arg(base).arg(n);

    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        watcher->deleteLater();
        prune();
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([image, path] { return image.save(path, "PNG"); }));
}

QList<QFileInfo> CaptureHistory::entries() const
{
    return QDir(m_folder).entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Name | QDir::Reversed);
}

QIcon CaptureHistory::thumbnail(const QFileInfo &entry)
{
    const QString key = entry.absoluteFilePath();
    auto cached = m_thumbnails.constFind(key);
    if (cached != m_thumbnails.constEnd())
        return *cached;
    QImageReader reader(key);
    const QSize size = reader.size();
    if (!size.isEmpty())
        reader.setScaledSize(size.scaled(QSize(96, 64), Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
    const QImage image = reader.read();
    if (image.isNull())
        return {};
    const QIcon icon(QPixmap::fromImage(image));
    m_thumbnails.insert(key, icon);
    return icon;
}

void CaptureHistory::prune()
{
    const QList<QFileInfo> files = entries();
    for (qsizetype i = qMax(0, Config::historySize()); i < files.size(); ++i) {
        m_thumbnails.remove(files[i].absoluteFilePath());
        QFile::remove(files[i].absoluteFilePath());
    }
}

void CaptureHistory::clear()
{
    for (const QFileInfo &entry : entries())
        QFile::remove(entry.absoluteFilePath());
    m_thumbnails.clear();
    emit changed();
}
