#include "windowfinder.h"

#include <windows.h>
#include <dwmapi.h>

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

} // namespace

QRect foregroundWindowRect()
{
    HWND window = GetForegroundWindow();
    if (!window || window == GetShellWindow())
        return {};
    wchar_t className[64] = {};
    GetClassNameW(window, className, 64);
    if (wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"WorkerW") == 0
        || wcscmp(className, L"Progman") == 0)
        return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() || IsIconic(window))
        return {};
    return frameBounds(window);
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
