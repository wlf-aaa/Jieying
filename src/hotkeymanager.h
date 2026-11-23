#pragma once

#include "config.h"

#include <QObject>

class GlobalHotkeys;

// Keeps the configured global hotkeys registered and validates changes to them.
class HotkeyManager : public QObject
{
    Q_OBJECT

public:
    explicit HotkeyManager(QObject *parent = nullptr);

    // Registers every configured hotkey; returns the ones another program already holds.
    QList<Config::Hotkey> registerAll();
    bool isRegistered(Config::Hotkey hotkey) const;

    // Assigns (or, with an empty sequence, clears) a hotkey. On success the new key is active
    // and saved; otherwise the old one stays and the reason is returned.
    QString change(Config::Hotkey hotkey, const QKeySequence &sequence);

    // While suspended no hotkey is registered, so a key-capture field can receive e.g. F1
    // instead of the system routing it to us as a hotkey. change() still validates and saves.
    void setSuspended(bool suspended);

    // The foreground application covers a screen. Keep this pause independent of key editing.
    void setPausedForFullscreen(bool paused);
    bool pausedForFullscreen() const { return m_pausedForFullscreen; }

    // Replaces every key with a scheme's set in one go (no transient duplicate errors) and
    // registers them; returns the ones another program already holds.
    QList<Config::Hotkey> applyScheme(Config::HotkeyScheme scheme);

signals:
    void triggered(Config::Hotkey hotkey);
    void changed();
    void fullscreenPauseChanged();

private:
    void unregisterAll();
    bool registrationPaused() const { return m_suspended || m_pausedForFullscreen; }
    void refreshAfterPauseChange(bool wasPaused);

    GlobalHotkeys *m_keys;
    QList<Config::Hotkey> m_registered;
    bool m_suspended = false;
    bool m_pausedForFullscreen = false;
};
