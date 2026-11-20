#include "globalhotkeys.h"

#include <QCoreApplication>

#include <windows.h>

namespace {

bool toNative(const QKeySequence &sequence, UINT &modifiers, UINT &vk)
{
    if (sequence.isEmpty())
        return false;

    const QKeyCombination combo = sequence[0];
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    modifiers = MOD_NOREPEAT;
    if (mods & Qt::ControlModifier)
        modifiers |= MOD_CONTROL;
    if (mods & Qt::AltModifier)
        modifiers |= MOD_ALT;
    if (mods & Qt::ShiftModifier)
        modifiers |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)
        modifiers |= MOD_WIN;

    const int key = combo.key();
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        vk = VK_F1 + UINT(key - Qt::Key_F1);
    else if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
        vk = UINT(key); // Qt and Win32 share ASCII codes for these
    else if (key == Qt::Key_Print)
        vk = VK_SNAPSHOT;
    else
        return false;
    return true;
}

} // namespace

GlobalHotkeys::GlobalHotkeys(QObject *parent)
    : QObject(parent)
{
    QCoreApplication::instance()->installNativeEventFilter(this);
}

GlobalHotkeys::~GlobalHotkeys()
{
    QCoreApplication::instance()->removeNativeEventFilter(this);
    for (int id : std::as_const(m_ids))
        UnregisterHotKey(nullptr, id);
}

bool GlobalHotkeys::add(int id, const QKeySequence &sequence)
{
    UINT modifiers = 0;
    UINT vk = 0;
    if (!toNative(sequence, modifiers, vk) || !RegisterHotKey(nullptr, id, modifiers, vk))
        return false;
    m_ids.append(id);
    return true;
}

void GlobalHotkeys::remove(int id)
{
    if (m_ids.removeAll(id) > 0)
        UnregisterHotKey(nullptr, id);
}

bool GlobalHotkeys::isSupported(const QKeySequence &sequence)
{
    UINT modifiers = 0;
    UINT vk = 0;
    return toNative(sequence, modifiers, vk);
}

bool GlobalHotkeys::nativeEventFilter(const QByteArray &, void *message, qintptr *)
{
    const MSG *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_HOTKEY || msg->hwnd != nullptr || !m_ids.contains(int(msg->wParam)))
        return false;
    emit activated(int(msg->wParam));
    return true;
}
