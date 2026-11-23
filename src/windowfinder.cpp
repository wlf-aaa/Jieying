#include "windowfinder.h"

#include <windows.h>
#include <dwmapi.h>

#include <string>

namespace {

constexpr int kMinChildSide = 8;
constexpr qsizetype kMaxChildrenPerWindow = 400;

QRect toQRect(const RECT &r)
{
    return QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
}

bool isCloaked(HWND hwnd)
{
    // UWP apps on other virtual desktops or suspended are "visible" but cloaked by DWM.
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

bool isCandidate(HWND hwnd, DWORD ownPid)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || isCloaked(hwnd))
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ownPid)
        return false;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TRANSPARENT) // click-through overlays (GPU/game HUDs) would shadow everything
        return false;
    if (exStyle & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0)
            return false;
    }
    return true;
}

QRect frameBounds(HWND hwnd)
{
    // Extended frame bounds exclude the invisible resize borders/shadow of Win10+ windows.
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        return toQRect(r);
    if (GetWindowRect(hwnd, &r))
        return toQRect(r);
    return {};
}

BOOL CALLBACK collectChild(HWND hwnd, LPARAM lParam)
{
    auto *node = reinterpret_cast<WindowNode *>(lParam);
    if (node->children.size() >= kMaxChildrenPerWindow)
        return FALSE;
    if (!IsWindowVisible(hwnd))
        return TRUE;

    RECT r{};
    if (!GetWindowRect(hwnd, &r))
        return TRUE;
    const QRect rect = toQRect(r) & node->rect;
    if (rect.width() >= kMinChildSide && rect.height() >= kMinChildSide && rect != node->rect)
        node->children.append(rect);
    return TRUE;
}

struct EnumContext
{
    DWORD ownPid;
    QList<WindowNode> *nodes;
};

BOOL CALLBACK collectTopLevel(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<EnumContext *>(lParam);
    if (!isCandidate(hwnd, ctx->ownPid))
        return TRUE;

    WindowNode node;
    node.rect = frameBounds(hwnd);
    if (node.rect.isEmpty())
        return TRUE;
    EnumChildWindows(hwnd, collectChild, reinterpret_cast<LPARAM>(&node));
    ctx->nodes->append(node);
    return TRUE;
}

qint64 area(const QRect &r)
{
    return qint64(r.width()) * r.height();
}

bool isShellWindow(HWND window)
{
    wchar_t className[64] = {};
    GetClassNameW(window, className, ARRAYSIZE(className));
    return window == GetShellWindow() || wcscmp(className, L"Shell_TrayWnd") == 0
        || wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Progman") == 0;
}

HWND foregroundAppWindow()
{
    HWND window = GetForegroundWindow();
    if (!window || isShellWindow(window))
        return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() || IsIconic(window))
        return nullptr;
    return window;
}

BOOL CALLBACK collectCaptureWindow(HWND hwnd, LPARAM data)
{
    if (!isCandidate(hwnd, GetCurrentProcessId()) || isShellWindow(hwnd))
        return TRUE;
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW))
        return TRUE;
    const int titleLength = GetWindowTextLengthW(hwnd);
    if (titleLength == 0 || frameBounds(hwnd).isEmpty())
        return TRUE;

    std::wstring title(size_t(titleLength) + 1, L'\0');
    const int length = GetWindowTextW(hwnd, title.data(), int(title.size()));
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (length > 0 && tid && pid != GetCurrentProcessId()) {
        auto *windows = reinterpret_cast<QList<CaptureWindow> *>(data);
        windows->append({reinterpret_cast<quintptr>(hwnd), quint32(pid), quint32(tid),
                         QString::fromWCharArray(title.data(), length)});
    }
    return TRUE;
}

} // namespace

QRect foregroundWindowRect()
{
    const HWND window = foregroundAppWindow();
    return window ? frameBounds(window) : QRect();
}

bool foregroundAppIsFullscreen()
{
    const HWND window = foregroundAppWindow();
    if (!window || !IsWindowVisible(window) || isCloaked(window))
        return false;
    // A normal maximized window can fill the whole monitor when the taskbar auto-hides.
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (IsZoomed(window) && (style & WS_CAPTION) == WS_CAPTION)
        return false;

    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return false;
    return frameBounds(window).contains(toQRect(info.rcMonitor));
}

QList<WindowNode> snapshotWindows()
{
    QList<WindowNode> nodes;
    EnumContext ctx{GetCurrentProcessId(), &nodes};
    EnumWindows(collectTopLevel, reinterpret_cast<LPARAM>(&ctx)); // enumerates in Z-order, topmost first
    return nodes;
}

QRect smallestRegionAt(const QList<WindowNode> &windows, const QPoint &pos)
{
    for (const WindowNode &node : windows) {
        if (!node.rect.contains(pos))
            continue;
        QRect best = node.rect;
        for (const QRect &child : node.children) {
            if (child.contains(pos) && area(child) < area(best))
                best = child;
        }
        return best;
    }
    return {};
}

QList<CaptureWindow> snapshotCaptureWindows()
{
    QList<CaptureWindow> windows;
    EnumWindows(collectCaptureWindow, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

CaptureWindowState captureWindowState(const CaptureWindow &window)
{
    const HWND hwnd = reinterpret_cast<HWND>(window.handle);
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || tid != window.threadId || pid != window.processId
        || pid == GetCurrentProcessId())
        return CaptureWindowState::Closed;
    if (IsIconic(hwnd))
        return CaptureWindowState::Minimized;
    if (!IsWindowVisible(hwnd) || isCloaked(hwnd))
        return CaptureWindowState::Unavailable;
    return CaptureWindowState::Available;
}
