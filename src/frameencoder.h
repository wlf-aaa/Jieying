#pragma once

#include <QFile>
#include <QImage>
#include <QSize>
#include <QString>

// Streams timestamped frames of a fixed size into a video/animation file.
class FrameEncoder
{
public:
    virtual ~FrameEncoder() = default;

    virtual bool open(const QString &path, const QSize &size, int fps) = 0;
    // Frames must arrive in timestamp order; they are cropped/padded to the opened size.
    virtual bool addFrame(const QImage &frame, qint64 timestampMs) = 0;
    // 48 kHz interleaved int16 stereo starting at firstFrame (48 kHz frames since the start).
    // Formats without sound ignore it.
    virtual bool addAudio(const QByteArray &pcm, qint64 firstFrame)
    {
        Q_UNUSED(pcm);
        Q_UNUSED(firstFrame);
        return true;
    }
    // endMs is when the last frame stops being shown.
    virtual bool finish(qint64 endMs) = 0;

    QString errorString() const { return m_error; }

protected:
    bool fail(const QString &error)
    {
        m_error = error;
        return false;
    }

    QString m_error;
};

// Animated GIF: per-frame median-cut palette, only the changed rectangle is stored, loops forever.
class GifEncoder : public FrameEncoder
{
public:
    bool open(const QString &path, const QSize &size, int fps) override;
    bool addFrame(const QImage &frame, qint64 timestampMs) override;
    bool finish(qint64 endMs) override;

private:
    bool writePending(qint64 untilMs);

    QFile m_file;
    QSize m_size;
    QImage m_shown;   // what the viewer displays after the last written frame
    QImage m_pending; // held back until the next frame tells how long it lasts
    qint64 m_pendingMs = 0;
    qint64 m_writtenCs = 0; // total delay written so far, to keep rounding from drifting
};

// H.264 MP4 through Media Foundation's sink writer (hardware encoder when available).
class Mp4Encoder : public FrameEncoder
{
public:
    Mp4Encoder();
    ~Mp4Encoder() override;

    // Average bitrate as bits per pixel per frame; call before open().
    void setBitsPerPixel(double bitsPerPixel);
    // Adds an AAC sound track (48 kHz stereo); call before open(). If the system has no AAC
    // encoder the video is written without sound.
    void setAudio(bool on);
    bool hasAudio() const; // after open(): whether the sound track exists

    bool open(const QString &path, const QSize &size, int fps) override;
    bool addFrame(const QImage &frame, qint64 timestampMs) override;
    bool addAudio(const QByteArray &pcm, qint64 firstFrame) override;
    bool finish(qint64 endMs) override;

private:
    void release();
    bool writeSample(const QImage &frame, qint64 timestampMs, qint64 durationMs);

    struct Private;
    Private *d;
};

namespace Gif {

// Exposed for tests. Maps every pixel to an index into a palette of at most 256 colors.
struct Quantized
{
    QList<QRgb> palette;
    QByteArray indices; // one byte per pixel, row-major
};
Quantized quantize(const QImage &image);

// GIF-flavoured LZW of 8-bit indices, packed into data sub-blocks with terminator.
QByteArray lzwCompress(const QByteArray &indices);

} // namespace Gif
