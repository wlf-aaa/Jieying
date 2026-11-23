#pragma once

#include <QImage>
#include <QRect>
#include <QString>

#include <memory>

// Grabs a screen region through Windows.Graphics.Capture: the compositor hands GPU frames of the
// whole monitor, and grab() copies just the region. Cheaper than GDI for large regions and high
// frame rates. On Windows 10 the system draws a yellow border around the captured monitor;
// Windows 11 lets us hide it (see canHideBorder()).
class WgcGrabber
{
public:
    WgcGrabber();
    ~WgcGrabber();

    static bool isSupported();
    // Whether this Windows version can capture without the yellow border.
    static bool canHideBorder();

    // physicalRect: global physical pixels within one monitor. Returns false (see error())
    // if capture cannot start; callers then fall back to GDI.
    bool start(const QRect &physicalRect, bool includeCursor);
    // Latest frame's region. A static screen yields no new frames, so the previous one is reused.
    QImage grab();
    void stop();

    QString error() const { return m_error; }

private:
    struct Private;
    std::unique_ptr<Private> d;
    QString m_error;
};

struct WindowCaptureFrame
{
    enum class Status { NoFrame, Frame, Resizing, Closed, Error };

    Status status = Status::NoFrame;
    QImage image;
};

// Window-only WGC on the calling thread. Polling never waits for a new frame or
// for the GPU readback. A frame's size is always in source pixels.
class WgcWindowGrabber
{
public:
    WgcWindowGrabber();
    ~WgcWindowGrabber();

    static bool isSupported();
    bool start(quintptr window);
    WindowCaptureFrame poll();
    void stop();
    QString error() const { return m_error; }

private:
    struct Private;
    std::unique_ptr<Private> d;
    QString m_error;
};
