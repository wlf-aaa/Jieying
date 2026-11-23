#include "screengrab.h"

#include "annotations.h"

#include <QPainter>
#include <QWidget>

#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

void drawCursor(HDC dc, const QRect &rect)
{
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING))
        return;
    ICONINFO icon{};
    if (!GetIconInfo(info.hCursor, &icon))
        return;
    DrawIconEx(dc, info.ptScreenPos.x - int(icon.xHotspot) - rect.x(), info.ptScreenPos.y - int(icon.yHotspot) - rect.y(),
               info.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    if (icon.hbmMask)
        DeleteObject(icon.hbmMask);
    if (icon.hbmColor)
        DeleteObject(icon.hbmColor);
}

} // namespace

QImage ScreenshotArea::render(const QImage &screen, const AnnotationStack *annotations) const
{
    if (bounds.isEmpty() || !screen.rect().contains(bounds))
        return {};

    QImage image = mask.isEmpty() ? screen.copy(bounds)
                                  : QImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull() || (mask.isEmpty() && !annotations))
        return image;
    if (!mask.isEmpty())
        image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.translate(-bounds.topLeft());
    if (!mask.isEmpty()) {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipPath(mask);
        painter.drawImage(bounds.topLeft(), screen, bounds);
    }
    if (annotations)
        annotations->paint(painter, bounds);
    painter.end();
    return image;
}

QImage grabScreenRect(const QRect &rect, bool includeCursor)
{
    if (rect.isEmpty())
        return {};

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.width();
    info.bmiHeader.biHeight = -rect.height(); // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

    QImage image;
    if (dib) {
        HGDIOBJ previous = SelectObject(memory, dib);
        BitBlt(memory, 0, 0, rect.width(), rect.height(), screen, rect.x(), rect.y(), SRCCOPY);
        if (includeCursor)
            drawCursor(memory, rect);
        GdiFlush();

        image = QImage(static_cast<const uchar *>(bits), rect.width(), rect.height(), rect.width() * 4,
                       QImage::Format_RGB32)
                    .copy();
        // GDI leaves the alpha byte at 0; RGB32 expects 0xFF.
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                row[x] |= 0xFF000000u;
        }
        SelectObject(memory, previous);
        DeleteObject(dib);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return image;
}

bool excludeFromCapture(QWidget *window)
{
    return SetWindowDisplayAffinity(reinterpret_cast<HWND>(window->winId()), WDA_EXCLUDEFROMCAPTURE);
}
