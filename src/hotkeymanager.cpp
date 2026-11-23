#include "hotkeymanager.h"

#include "globalhotkeys.h"

HotkeyManager::HotkeyManager(QObject *parent)
    : QObject(parent)
    , m_keys(new GlobalHotkeys(this))
{
    connect(m_keys, &GlobalHotkeys::activated, this, [this](int id) { emit triggered(Config::Hotkey(id)); });
}

void HotkeyManager::unregisterAll()
{
    for (Config::Hotkey hotkey : Config::kHotkeys)
        m_keys->remove(int(hotkey));
    m_registered.clear();
}

QList<Config::Hotkey> HotkeyManager::registerAll()
{
    unregisterAll();
    QList<Config::Hotkey> taken;
    if (registrationPaused())
        return taken;
    for (Config::Hotkey hotkey : Config::kHotkeys) {
        const QKeySequence sequence = Config::hotkey(hotkey);
        if (sequence.isEmpty())
            continue;
        if (m_keys->add(int(hotkey), sequence))
            m_registered.append(hotkey);
        else
            taken.append(hotkey);
    }
    return taken;
}

bool HotkeyManager::isRegistered(Config::Hotkey hotkey) const
{
    return m_registered.contains(hotkey);
}

QList<Config::Hotkey> HotkeyManager::applyScheme(Config::HotkeyScheme scheme)
{
    unregisterAll();
    for (Config::Hotkey hotkey : Config::kHotkeys)
        Config::setHotkey(hotkey, Config::schemeHotkey(scheme, hotkey));
    const QList<Config::Hotkey> taken = registerAll();
    emit changed();
    return taken;
}

void HotkeyManager::setSuspended(bool suspended)
{
    if (suspended == m_suspended)
        return;
    const bool wasPaused = registrationPaused();
    m_suspended = suspended;
    refreshAfterPauseChange(wasPaused);
}

void HotkeyManager::setPausedForFullscreen(bool paused)
{
    if (paused == m_pausedForFullscreen)
        return;
    const bool wasPaused = registrationPaused();
    m_pausedForFullscreen = paused;
    refreshAfterPauseChange(wasPaused);
    emit fullscreenPauseChanged();
}

void HotkeyManager::refreshAfterPauseChange(bool wasPaused)
{
    const bool paused = registrationPaused();
    if (paused == wasPaused)
        return;
    if (paused)
        unregisterAll();
    else
        registerAll();
}

QString HotkeyManager::change(Config::Hotkey hotkey, const QKeySequence &sequence)
{
    const QKeySequence previous = Config::hotkey(hotkey);
    if (sequence == previous && (sequence.isEmpty() || registrationPaused() || isRegistered(hotkey)))
        return {};

    if (!sequence.isEmpty()) {
        if (!GlobalHotkeys::isSupported(sequence))
            return tr("不支持这个按键，请用 F1–F24、字母、数字或 PrintScreen，可加 Ctrl/Alt/Shift/Win");
        for (Config::Hotkey other : Config::kHotkeys) {
            if (other != hotkey && Config::hotkey(other) == sequence)
                return tr("和「%1」的快捷键重复").arg(Config::hotkeyName(other));
        }
    }

    // Try the new key for real even while suspended, so "taken by another program" shows now.
    m_keys->remove(int(hotkey));
    m_registered.removeAll(hotkey);
    if (!sequence.isEmpty()) {
        if (!m_keys->add(int(hotkey), sequence)) {
            if (!registrationPaused() && !previous.isEmpty() && m_keys->add(int(hotkey), previous)) // keep the old key
                m_registered.append(hotkey);
            return tr("已被其他程序占用");
        }
        if (registrationPaused())
            m_keys->remove(int(hotkey));
        else
            m_registered.append(hotkey);
    }
    Config::setHotkey(hotkey, sequence);
    emit changed();
    return {};
}
