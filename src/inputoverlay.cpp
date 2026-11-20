#include "inputoverlay.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>

#include <atomic>

#include <windows.h>

namespace {

constexpr qint64 kRingMs = 450;     // the ring keeps growing this long after a press
constexpr qint64 kCaptionMs = 1500; // how long a shortcut stays on screen
const QColor kLeft(30, 144, 255);
const QColor kRight(255, 149, 0);

// One overlay records at a time; the hook procedures (on its hook thread) reach it through this.
std::atomic<InputOverlay *> g_active = nullptr;

LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    InputOverlay *active = g_active;
    if (code == HC_ACTION && active) {
        const auto *info = reinterpret_cast<const MSLLHOOKSTRUCT *>(lParam);
        const QPoint pos(info->pt.x, info->pt.y);
        switch (wParam) {
        case WM_LBUTTONDOWN:
            active->addPress(pos, false);
            break;
        case WM_RBUTTONDOWN:
            active->addPress(pos, true);
            break;
        case WM_LBUTTONUP:
            active->addRelease(false);
            break;
        case WM_RBUTTONUP:
            active->addRelease(true);
            break;
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool down(int vk)
{
    return GetAsyncKeyState(vk) & 0x8000;
}

LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    InputOverlay *active = g_active;
    if (code == HC_ACTION && active && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const auto *info = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
        active->addKey(info->vkCode, down(VK_CONTROL), down(VK_MENU), down(VK_SHIFT),
                         down(VK_LWIN) || down(VK_RWIN));
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

QString keyName(quint32 vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return QString(QChar(char(vk)));
    if (vk >= VK_F1 && vk <= VK_F24)
        return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return QStringLiteral("Num %1").arg(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_RETURN: return QStringLiteral("Enter");
    case VK_ESCAPE: return QStringLiteral("Esc");
    case VK_TAB: return QStringLiteral("Tab");
    case VK_SPACE: return QStringLiteral("Space");
    case VK_BACK: return QStringLiteral("Backspace");
    case VK_DELETE: return QStringLiteral("Delete");
    case VK_INSERT: return QStringLiteral("Insert");
    case VK_HOME: return QStringLiteral("Home");
    case VK_END: return QStringLiteral("End");
    case VK_PRIOR: return QStringLiteral("PgUp");
    case VK_NEXT: return QStringLiteral("PgDn");
    case VK_LEFT: return QStringLiteral("←");
    case VK_RIGHT: return QStringLiteral("→");
    case VK_UP: return QStringLiteral("↑");
    case VK_DOWN: return QStringLiteral("↓");
    case VK_SNAPSHOT: return QStringLiteral("PrtSc");
    case VK_OEM_PLUS: return QStringLiteral("+");
    case VK_OEM_MINUS: return QStringLiteral("-");
    case VK_OEM_COMMA: return QStringLiteral(",");
    case VK_OEM_PERIOD: return QStringLiteral(".");
    case VK_OEM_1: return QStringLiteral(";");
    case VK_OEM_2: return QStringLiteral("/");
    case VK_OEM_3: return QStringLiteral("`");
    case VK_OEM_4: return QStringLiteral("[");
    case VK_OEM_5: return QStringLiteral("\\");
    case VK_OEM_6: return QStringLiteral("]");
    case VK_OEM_7: return QStringLiteral("'");
    default: return {};
    }
}

bool isModifier(quint32 vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return true;
    default:
        return false;
    }
}

// Keys that type a character on their own; shown only as part of a shortcut.
bool typesText(quint32 vk)
{
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)
           || (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102
           || vk == VK_SPACE;
}

} // namespace

InputOverlay::InputOverlay()
{
    m_clock.start();
}

InputOverlay::~InputOverlay()
{
    stop();
}

void InputOverlay::start(bool clicks, bool keys)
{
    stop();
    if (!clicks && !keys)
        return;
    g_active = this;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_hookThread = std::thread([this, clicks, keys, ready] {
        // Hooks are called on the thread that set them, from its message loop.
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // gives the thread its queue
        m_hookThreadId = GetCurrentThreadId();
        const HINSTANCE module = GetModuleHandleW(nullptr);
        HHOOK mouse = clicks ? SetWindowsHookExW(WH_MOUSE_LL, mouseProc, module, 0) : nullptr;
        HHOOK keyboard = keys ? SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, module, 0) : nullptr;
        SetEvent(ready);
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            DispatchMessageW(&msg);
        if (mouse)
            UnhookWindowsHookEx(mouse);
        if (keyboard)
            UnhookWindowsHookEx(keyboard);
    });
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
}

void InputOverlay::stop()
{
    // Unhooks before the overlay can go away: the hook procedures call into it.
    if (m_hookThread.joinable()) {
        PostThreadMessageW(m_hookThreadId, WM_QUIT, 0, 0);
        m_hookThread.join();
    }
    InputOverlay *self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

void InputOverlay::addPress(const QPoint &pos, bool right)
{
    const std::lock_guard lock(m_mutex);
    const qint64 t = now();
    m_clicks.removeIf([t](const Click &c) { return c.releasedMs >= 0 && t - c.pressedMs > kRingMs; });
    m_clicks.append({pos, t, -1, right});
}

void InputOverlay::addRelease(bool right)
{
    const std::lock_guard lock(m_mutex);
    for (Click &c : m_clicks) {
        if (c.right == right && c.releasedMs < 0)
            c.releasedMs = now();
    }
}

void InputOverlay::addKey(quint32 virtualKey, bool ctrl, bool alt, bool shift, bool win)
{
    const QString caption = describeKey(virtualKey, ctrl, alt, shift, win);
    if (caption.isEmpty())
        return;
    const std::lock_guard lock(m_mutex);
    m_caption = caption;
    m_captionMs = now();
}

QString InputOverlay::describeKey(quint32 virtualKey, bool ctrl, bool alt, bool shift, bool win)
{
    if (isModifier(virtualKey))
        return {};
    const bool shortcut = ctrl || alt || win;
    if (!shortcut && typesText(virtualKey))
        return {}; // typing (Shift only makes capitals): never shown
    const QString name = keyName(virtualKey);
    if (name.isEmpty())
        return {};
    QStringList parts;
    if (ctrl)
        parts << QStringLiteral("Ctrl");
    if (alt)
        parts << QStringLiteral("Alt");
    if (shift)
        parts << QStringLiteral("Shift");
    if (win)
        parts << QStringLiteral("Win");
    parts << name;
    return parts.join(QStringLiteral(" + "));
}

void InputOverlay::paint(QImage &frame, const QPoint &origin) const
{
    const std::lock_guard lock(m_mutex);
    const qint64 t = now();
    const bool anyClick = std::any_of(m_clicks.cbegin(), m_clicks.cend(), [t](const Click &c) {
        return c.releasedMs < 0 || t - c.pressedMs <= kRingMs;
    });
    const bool caption = !m_caption.isEmpty() && m_captionMs >= 0 && t - m_captionMs <= kCaptionMs;
    if (!anyClick && !caption)
        return;
    if (frame.format() != QImage::Format_RGB32 && frame.format() != QImage::Format_ARGB32_Premultiplied)
        frame = frame.convertToFormat(QImage::Format_RGB32);

    QPainter p(&frame);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal unit = qMax(1.0, frame.height() / 720.0); // grows with tall regions
    for (const Click &c : m_clicks) {
        const QPointF at = QPointF(c.pos - origin);
        const QColor color = c.right ? kRight : kLeft;
        const qint64 age = t - c.pressedMs;
        if (c.releasedMs < 0) { // held: a soft dot under the cursor
            QColor fill = color;
            fill.setAlpha(90);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawEllipse(at, 14 * unit, 14 * unit);
        }
        if (age <= kRingMs) { // a ring spreading out from the press
            const qreal progress = qreal(age) / kRingMs;
            QColor ring = color;
            ring.setAlphaF(0.85 * (1 - progress));
            p.setPen(QPen(ring, 3 * unit));
            p.setBrush(Qt::NoBrush);
            const qreal radius = (10 + 22 * progress) * unit;
            p.drawEllipse(at, radius, radius);
        }
    }

    if (caption) {
        QFont font = QGuiApplication::font();
        font.setPixelSize(qRound(22 * unit));
        font.setBold(true);
        p.setFont(font);
        const QFontMetricsF fm(font);
        const qreal fade = qBound(0.0, (kCaptionMs - (t - m_captionMs)) / 300.0, 1.0); // fades in the last 300 ms
        QRectF box(0, 0, fm.horizontalAdvance(m_caption) + 32 * unit, fm.height() + 16 * unit);
        box.moveCenter(QPointF(frame.width() / 2.0, frame.height() - box.height() / 2 - 24 * unit));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, int(190 * fade)));
        p.drawRoundedRect(box, box.height() / 2, box.height() / 2);
        p.setPen(QColor(255, 255, 255, int(255 * fade)));
        p.drawText(box, Qt::AlignCenter, m_caption);
    }
}
