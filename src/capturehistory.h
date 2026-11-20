#pragma once

#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QObject>

// Keeps the most recent captures as PNG files (Config::historySize(), 0 = off) so they can be
// pinned again later. Writing happens on a worker thread; changed() follows each write.
class CaptureHistory : public QObject
{
    Q_OBJECT

public:
    explicit CaptureHistory(const QString &folder, QObject *parent = nullptr);

    // %LOCALAPPDATA%\Jieying\Jieying\history, or data\history next to a portable copy.
    static QString defaultFolder();

    QString folder() const { return m_folder; }
    void add(const QImage &image);
    QList<QFileInfo> entries() const; // newest first
    QIcon thumbnail(const QFileInfo &entry);
    void clear();
    // Deletes the oldest files beyond the configured size.
    void prune();

signals:
    void changed();

private:
    QString m_folder;
    QHash<QString, QIcon> m_thumbnails;
};
