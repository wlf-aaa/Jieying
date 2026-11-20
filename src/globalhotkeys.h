#pragma once

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QList>
#include <QObject>

// System-wide hotkeys via RegisterHotKey. WM_HOTKEY is posted to the GUI thread's queue
// (no window), so it is picked up from Qt's dispatcher through a native event filter.
class GlobalHotkeys : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit GlobalHotkeys(QObject *parent = nullptr);
    ~GlobalHotkeys() override;

    // False if the key is unsupported or already taken by another program.
    bool add(int id, const QKeySequence &sequence);
    void remove(int id);

    // Whether the sequence maps to a registrable key (F-keys, letters, digits, Print Screen).
    static bool isSupported(const QKeySequence &sequence);

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void activated(int id);

private:
    QList<int> m_ids;
};
