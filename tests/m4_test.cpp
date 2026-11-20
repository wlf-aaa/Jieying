// Headless tests for long screenshots and recording: the scroll stitcher on a synthetic page,
// GIF output decoded back with Qt's reader, MP4 output decoded back with Media Foundation.
// Usage: m4_test [output-dir]
#include "audiocapture.h"
#include "autoscroller.h"
#include "config.h"
#include "frameencoder.h"
#include "inputoverlay.h"
#include "recorder.h"
#include "scrollcapture.h"
#include "scrollstitcher.h"
#include "screengrab.h"
#include "wgcgrabber.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImageReader>
#include <QPainter>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

bool colorsClose(QRgb a, QRgb b, int tolerance)
{
    return qAbs(qRed(a) - qRed(b)) <= tolerance && qAbs(qGreen(a) - qGreen(b)) <= tolerance
           && qAbs(qBlue(a) - qBlue(b)) <= tolerance;
}

// ---- scroll stitching -------------------------------------------------------------------

constexpr int kPageWidth = 600;
constexpr int kHeader = 40;    // sticky header, same in every frame
constexpr int kViewport = 460; // scrolling part of the frame
constexpr int kScrollbar = 12;

QImage makePage()
{
    QImage page(kPageWidth, 2400, QImage::Format_RGB32);
    page.fill(Qt::white);
    QPainter p(&page);
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(14);
    p.setFont(font);
    for (int y = 20, n = 1; y < page.height(); y += 22, ++n) {
        if (n % 9 == 0)
            continue; // blank lines: uniform rows must not confuse matching
        p.setPen(QColor((n * 37) % 160, (n * 11) % 120, (n * 53) % 200));
        p.drawText(16, y, QStringLiteral("第 %1 行 line %1 — the quick brown fox %2").arg(n).arg(n * n));
    }
    p.fillRect(QRect(300, 700, 200, 150), QColor(255, 180, 0)); // a picture block
    p.fillRect(QRect(320, 720, 60, 60), QColor(0, 120, 255));
    return page;
}

QImage viewportAt(const QImage &page, int offset)
{
    QImage frame(kPageWidth, kHeader + kViewport, QImage::Format_RGB32);
    QPainter p(&frame);
    p.drawImage(QPoint(0, kHeader), page, QRect(0, offset, kPageWidth, kViewport));
    p.fillRect(QRect(0, 0, kPageWidth, kHeader), QColor(30, 60, 120));
    p.setPen(Qt::white);
    p.drawText(QRect(0, 0, kPageWidth, kHeader), Qt::AlignCenter, QStringLiteral("Sticky header"));
    // Scrollbar whose thumb moves with the offset, as in a real window.
    p.fillRect(QRect(kPageWidth - kScrollbar, 0, kScrollbar, frame.height()), QColor(235, 235, 235));
    const int thumb = offset * (frame.height() - 60) / (page.height() - kViewport);
    p.fillRect(QRect(kPageWidth - kScrollbar + 2, thumb, kScrollbar - 4, 60), QColor(150, 150, 150));
    return frame;
}

void testStitcher(const QDir &out)
{
    const QImage page = makePage();
    ScrollStitcher stitcher;
    using S = ScrollStitcher::Status;

    check(stitcher.add(viewportAt(page, 0)) == S::Started, "stitcher: first frame starts");
    check(stitcher.add(viewportAt(page, 0)) == S::Unchanged, "stitcher: identical frame is unchanged");
    bool appended = true;
    for (int offset : {120, 260, 261, 500, 900, 1300})
        appended = appended && stitcher.add(viewportAt(page, offset)) == S::Appended;
    check(appended, "stitcher: scrolls of 1..400 px are appended");
    check(stitcher.add(viewportAt(page, 1300 + kViewport + 10)) == S::Lost, "stitcher: a jump without overlap is lost");
    check(stitcher.add(viewportAt(page, 1700)) == S::Appended, "stitcher: recovers when overlap returns");
    stitcher.add(viewportAt(page, 1940)); // end of page

    const QImage result = stitcher.result();
    QImage expected(kPageWidth, kHeader + 1940 + kViewport, QImage::Format_RGB32);
    {
        QPainter p(&expected);
        p.drawImage(0, 0, viewportAt(page, 0).copy(0, 0, kPageWidth, kHeader));
        p.drawImage(QPoint(0, kHeader), page, QRect(0, 0, kPageWidth, 1940 + kViewport));
    }
    // Compare everything left of the scrollbar.
    const QRect compared(0, 0, kPageWidth - 40, expected.height());
    check(result.size() == expected.size(), "stitcher: result height = header + whole page");
    check(result.size() == expected.size() && result.copy(compared) == expected.copy(compared),
          "stitcher: result pixels equal the page with one header");
    result.save(out.filePath("m4_stitched.png"));

    ScrollStitcher capped(800);
    capped.add(viewportAt(page, 0));
    capped.add(viewportAt(page, 400));
    check(capped.add(viewportAt(page, 800)) == S::Full && capped.height() == 800, "stitcher: stops at max height");
}

// Scrolling right: the synthetic page mirrored along its diagonal scrolls sideways, with the
// sticky header as a fixed left column and the scrollbar along the bottom.
void testSidewaysStitch(const QDir &out)
{
    using S = ScrollStitcher::Status;
    using D = TwoWayStitcher::Direction;
    const QTransform transpose(0, 1, 1, 0, 0, 0);
    const QImage page = makePage();
    const int offsets[] = {0, 0, 120, 260, 500, 900, 1300, 1700, 1940};

    TwoWayStitcher down(30000, 30000);
    ScrollStitcher reference;
    for (int offset : offsets) {
        down.add(viewportAt(page, offset));
        reference.add(viewportAt(page, offset));
    }
    check(down.direction() == D::Down && down.result() == reference.result(), "two-way: scrolling down works as before");

    TwoWayStitcher sideways(30000, 30000);
    QList<S> statuses;
    for (int offset : offsets)
        statuses << sideways.add(viewportAt(page, offset).transformed(transpose));
    check(statuses[0] == S::Started && statuses[1] == S::Unchanged && statuses[2] == S::Appended,
          "two-way: first frame, no change, then appended");
    const QImage wide = sideways.result();
    wide.save(out.filePath("m4_sideways.png"));
    std::printf("      sideways result %dx%d\n", wide.width(), wide.height());
    check(sideways.direction() == D::Right && sideways.length() == reference.height(), "two-way: scrolling right is recognised");
    check(wide == reference.result().transformed(transpose), "two-way: sideways result is the page, fixed column once");
    check(sideways.tail(100).size() == QSize(100, wide.height()), "two-way: preview shows the right end");
}

// Browsers and modern apps scroll by fractions of a pixel (smooth / percent-based scrolling),
// so text edges are anti-aliased differently in every frame and no row repeats exactly.
// Frames here are painted at fractional offsets, with a hover highlight that moves along
// and a small animation, as in a real page.
QImage smoothViewportAt(double offset, int frameNo)
{
    QImage frame(kPageWidth, kHeader + kViewport, QImage::Format_RGB32);
    frame.fill(Qt::white);
    QPainter p(&frame);
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipRect(QRect(0, kHeader, kPageWidth, kViewport));
    p.translate(0, kHeader - offset);
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(14);
    for (int y = 20, n = 1; y < 2400; y += 22, ++n) {
        if (y - offset < -30 || y - offset > kViewport + 30 || n % 9 == 0)
            continue;
        QPainterPath text;
        text.addText(16, y, font, QStringLiteral("第 %1 行 line %1 — the quick brown fox %2").arg(n).arg(n * n));
        p.fillPath(text, QColor((n * 37) % 160, (n * 11) % 120, (n * 53) % 200));
    }
    p.fillRect(QRectF(300, 700, 200, 150), QColor(255, 180, 0));
    p.fillRect(QRectF(320, 720, 60, 60), QColor(0, 120, 255));
    p.resetTransform();
    p.setClipping(false);
    p.fillRect(QRect(0, kHeader + 200, kPageWidth - 40, 22), QColor(30, 144, 255, 25)); // hover row under the mouse
    p.fillRect(QRect(500, kHeader + 10 + (frameNo % 3) * 4, 30, 8), QColor(255, 0, 0)); // a small animation
    p.fillRect(QRect(0, 0, kPageWidth, kHeader), QColor(30, 60, 120));
    p.setPen(Qt::white);
    p.drawText(QRect(0, 0, kPageWidth, kHeader), Qt::AlignCenter, QStringLiteral("Sticky header"));
    p.fillRect(QRect(kPageWidth - kScrollbar, 0, kScrollbar, frame.height()), QColor(235, 235, 235));
    p.fillRect(QRectF(kPageWidth - kScrollbar + 2, offset * 0.2, kScrollbar - 4, 60), QColor(150, 150, 150));
    return frame;
}

void testSmoothScrolling(const QDir &out)
{
    using S = ScrollStitcher::Status;
    ScrollStitcher stitcher;
    const double offsets[] = {0, 0, 37.5, 81.25, 130.6, 190.4, 260.75, 333.3, 421.9, 500.5, 610.2, 700.8, 811.4};
    QList<S> statuses;
    int frameNo = 0;
    for (double offset : offsets)
        statuses << stitcher.add(smoothViewportAt(offset, frameNo++));
    QStringList names;
    for (S status : statuses)
        names << QString::number(int(status));
    const double expected = kHeader + 811.4 + kViewport;
    std::printf("      statuses %s, height %d (expected about %.0f)\n", qPrintable(names.join(' ')), stitcher.height(), expected);
    stitcher.result().save(out.filePath("m4_smooth.png"));
    check(statuses[1] == S::Unchanged, "smooth scroll: a still frame with a tiny animation is unchanged");
    check(std::all_of(statuses.begin() + 2, statuses.end(), [](S s) { return s == S::Appended; }),
          "smooth scroll: fractional-pixel scrolls are all appended");
    check(qAbs(stitcher.height() - expected) <= 4, "smooth scroll: the result is as long as the page shown");

    // The newest rows (below the last seam) are the last frame's own pixels. Further up, each
    // seam may be off by up to half a pixel, invisibly, so no exact comparison there.
    const QImage result = stitcher.result();
    const QImage last = smoothViewportAt(811.4, 99);
    int differing = 0;
    for (int y = kHeader + kViewport - 100; y < kHeader + kViewport; ++y) {
        const int resultY = result.height() - (kHeader + kViewport) + y;
        if (resultY < 0 || resultY >= result.height())
            continue;
        for (int x = 16; x < 480; x += 3)
            differing += qAbs(qGray(result.pixel(x, resultY)) - qGray(last.pixel(x, y))) > 60;
    }
    std::printf("      differing samples in the last screen: %d\n", differing);
    check(differing == 0, "smooth scroll: the end of the result shows the last screen");

    // Speed on a large region (both directions tried, as before the direction is known): frames
    // are sampled every 100 ms, so matching must stay well below that in release builds.
    TwoWayStitcher big(30000, 30000);
    QList<QImage> frames;
    for (double offset : {0.0, 60.5, 133.25, 210.75})
        frames << smoothViewportAt(offset, 0).scaled(1400, 900);
    QElapsedTimer timer;
    timer.start();
    for (const QImage &frame : frames)
        big.add(frame);
    std::printf("      1400x900 region: %.0f ms per frame\n", timer.elapsed() / 4.0);
}

// ---- GIF ----------------------------------------------------------------------------------

QImage gifFrame(QColor background, QPoint square)
{
    QImage frame(160, 120, QImage::Format_RGB32);
    frame.fill(background);
    QPainter p(&frame);
    p.fillRect(QRect(square, QSize(30, 30)), QColor(20, 90, 220));
    p.setPen(Qt::white);
    p.drawText(QRect(0, 80, 160, 30), Qt::AlignCenter, QStringLiteral("Jietu GIF"));
    return frame;
}

void testGif(const QDir &out)
{
    const QList<std::pair<QImage, qint64>> frames = {
        {gifFrame(QColor(200, 40, 40), {10, 10}), 0},
        {gifFrame(QColor(200, 40, 40), {60, 20}), 100},
        {gifFrame(QColor(200, 40, 40), {60, 20}), 200}, // repeat: merged into the previous frame
        {gifFrame(QColor(40, 160, 60), {110, 30}), 300},
    };
    const QString path = out.filePath("m4_test.gif");
    GifEncoder encoder;
    bool ok = encoder.open(path, QSize(160, 120), 10);
    for (const auto &[frame, ms] : frames)
        ok = ok && encoder.addFrame(frame, ms);
    ok = ok && encoder.finish(500);
    check(ok, "gif: encodes without error");

    QImageReader reader(path);
    check(reader.canRead(), "gif: Qt's reader accepts the file");
    QList<QImage> decoded;
    QList<int> delays;
    while (reader.canRead()) {
        decoded << reader.read().convertToFormat(QImage::Format_RGB32);
        delays << reader.nextImageDelay();
    }
    std::printf("      %lld frames, delays", decoded.size());
    for (int d : delays)
        std::printf(" %d", d);
    std::printf(" ms, %lld bytes, loop %d\n", QFileInfo(path).size(), QImageReader(path).loopCount());
    check(decoded.size() == 3, "gif: the repeated frame is merged (3 frames)");
    check(delays == QList<int>({100, 200, 200}), "gif: delays follow the timestamps");
    check(QImageReader(path).loopCount() == -1, "gif: loops forever");

    // Each decoded frame must look like its source (15-bit palette: within a few levels).
    const QList<QImage> sources = {frames[0].first, frames[1].first, frames[3].first};
    bool same = decoded.size() == 3;
    for (int i = 0; same && i < 3; ++i) {
        for (const QPoint &pt : {QPoint(5, 5), QPoint(70, 30), QPoint(120, 40), QPoint(150, 115)})
            same = same && colorsClose(decoded[i].pixel(pt), sources[i].pixel(pt), 8);
    }
    check(same, "gif: decoded frames match the sources (delta frames composite correctly)");

    // LZW across a dictionary reset: 200k pseudo-random indices must round-trip through Qt's reader.
    QImage noise(500, 400, QImage::Format_RGB32);
    quint32 seed = 12345;
    for (int y = 0; y < noise.height(); ++y)
        for (int x = 0; x < noise.width(); ++x) {
            seed = seed * 1664525u + 1013904223u;
            noise.setPixel(x, y, qRgb((seed >> 8) & 0xF8, (seed >> 16) & 0xF8, (seed >> 24) & 0xF8));
        }
    const QString noisePath = out.filePath("m4_noise.gif");
    GifEncoder big;
    ok = big.open(noisePath, noise.size(), 10) && big.addFrame(noise, 0) && big.finish(100);
    const QImage back = QImage(noisePath).convertToFormat(QImage::Format_RGB32);
    int mismatched = 0;
    const Gif::Quantized q = Gif::quantize(noise);
    for (int y = 0; y < noise.height() && !back.isNull(); ++y)
        for (int x = 0; x < noise.width(); ++x)
            mismatched += back.pixel(x, y) != (q.palette[uchar(q.indices[y * noise.width() + x])] | 0xFF000000u);
    check(ok && !back.isNull() && mismatched == 0, "gif: large LZW stream (with table resets) decodes exactly");
}

// ---- MP4 ----------------------------------------------------------------------------------

QImage mp4Frame(int i)
{
    QImage frame(320, 240, QImage::Format_RGB32);
    QPainter p(&frame);
    p.fillRect(QRect(0, 0, 320, 120), QColor(230, 20, 20)); // top red
    p.fillRect(QRect(0, 120, 320, 120), QColor(20, 40, 230)); // bottom blue
    p.fillRect(QRect(20 + i * 5, 100, 40, 40), Qt::white);
    return frame;
}

void testMp4(const QDir &out)
{
    const QString path = out.filePath("m4_test.mp4");
    QFile::remove(path);
    constexpr int kFrames = 40;
    constexpr int kStepMs = 66;
    Mp4Encoder encoder;
    bool ok = encoder.open(path, QSize(320, 240), 15);
    for (int i = 0; ok && i < kFrames; ++i)
        ok = encoder.addFrame(mp4Frame(i), i * kStepMs);
    ok = ok && encoder.finish(kFrames * kStepMs);
    if (!ok)
        std::printf("      error: %s\n", qPrintable(encoder.errorString()));
    check(ok, "mp4: encodes without error");

    // Decode it back with Media Foundation and inspect the first frame and the duration.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    const QString nativePath = QDir::toNativeSeparators(path);
    HRESULT hr = MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(nativePath.utf16()), attributes.Get(), &reader);
    check(SUCCEEDED(hr), "mp4: Media Foundation opens the file");
    if (FAILED(hr))
        return;

    PROPVARIANT duration;
    PropVariantInit(&duration);
    reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
    const double seconds = duration.uhVal.QuadPart / 1e7;
    PropVariantClear(&duration);

    ComPtr<IMFMediaType> rgb;
    MFCreateMediaType(&rgb);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, rgb.Get());

    // Signed stride of the decoded RGB32 frames: negative means bottom-up rows.
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
    const LONG stride = LONG(MFGetAttributeUINT32(current.Get(), MF_MT_DEFAULT_STRIDE, width * 4));

    constexpr int kInspected = 5; // past any encoder warm-up
    int decodedFrames = 0;
    QRgb top = 0, bottom = 0;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            break;
        if (!sample)
            continue;
        if (decodedFrames++ == kInspected) {
            ComPtr<IMFMediaBuffer> buffer;
            sample->ConvertToContiguousBuffer(&buffer);
            BYTE *data = nullptr;
            DWORD length = 0;
            if (buffer && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                const BYTE *row0 = stride < 0 ? data + (height - 1) * DWORD(-stride) : data;
                if (length >= height * DWORD(qAbs(stride))) {
                    top = reinterpret_cast<const QRgb *>(row0 + 20 * stride)[160];
                    bottom = reinterpret_cast<const QRgb *>(row0 + 220 * stride)[160];
                }
                buffer->Unlock();
            }
        }
    }
    std::printf("      decoded %ux%u, stride %ld\n", width, height, long(stride));
    reader.Reset();
    MFShutdown();

    std::printf("      %d frames, %.2f s, %lld bytes, top #%06X bottom #%06X\n", decodedFrames, seconds,
                QFileInfo(path).size(), top & 0xFFFFFF, bottom & 0xFFFFFF);
    check(decodedFrames >= kFrames - 2, "mp4: all frames decode");
    check(qAbs(seconds - kFrames * kStepMs / 1000.0) < 0.3, "mp4: duration follows the timestamps");
    check(qRed(top) > 180 && qBlue(top) < 80 && qBlue(bottom) > 180 && qRed(bottom) < 80,
          "mp4: picture is upright with correct colors (red top, blue bottom)");
}

// Decodes an MP4's sound track to PCM: seconds of audio and the loudest sample (0..1).
// seconds < 0: no sound track.
std::pair<double, double> mp4Audio(const QString &path)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    double seconds = -1, peak = 0;
    ComPtr<IMFSourceReader> reader;
    const QString nativePath = QDir::toNativeSeparators(path);
    if (SUCCEEDED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, &reader))) {
        ComPtr<IMFMediaType> pcm;
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        if (SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, pcm.Get()))) {
            ComPtr<IMFMediaType> current;
            reader->GetCurrentMediaType(stream, &current);
            const UINT32 rate = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            const UINT32 channels = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
            qint64 samples = 0;
            for (;;) {
                DWORD flags = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample))
                    || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
                    break;
                if (!sample)
                    continue;
                ComPtr<IMFMediaBuffer> buffer;
                sample->ConvertToContiguousBuffer(&buffer);
                BYTE *data = nullptr;
                DWORD length = 0;
                if (buffer && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    const auto *values = reinterpret_cast<const qint16 *>(data);
                    for (DWORD i = 0; i < length / 2; ++i)
                        peak = qMax(peak, qAbs(values[i]) / 32768.0);
                    samples += length / 2;
                    buffer->Unlock();
                }
            }
            seconds = double(samples) / channels / rate;
        }
    }
    reader.Reset();
    MFShutdown();
    return {seconds, peak};
}

// 48 kHz stereo int16 sine chunk starting at frame `first`.
QByteArray sineChunk(qint64 first, int frames, double hz)
{
    QByteArray pcm(frames * 4, Qt::Uninitialized);
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const auto v = qint16(12000 * std::sin(2 * M_PI * hz * double(first + i) / 48000));
        out[2 * i] = out[2 * i + 1] = v;
    }
    return pcm;
}

void testAudioMixer()
{
    AudioMixer mixer(2);
    std::vector<float> mono(44100);
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = 0.5f * float(std::sin(2 * M_PI * 440 * double(i) / 44100));
    for (size_t at = 0; at < mono.size(); at += 441) // 10 ms packets, as devices deliver
        mixer.push(0, mono.data() + at, 441, 1, 44100);
    std::printf("      44.1 kHz second -> %d frames at 48 kHz\n", mixer.queuedFrames(0));
    check(qAbs(mixer.queuedFrames(0) - 48000) <= 2, "audio mixer: resamples 44.1 kHz to 48 kHz");

    mixer.pull(100);
    check(mixer.queuedFrames(0) <= 48000 / 5, "audio mixer: a source far ahead is trimmed to 200 ms");

    AudioMixer pair(2);
    const float quiet[] = {0.1f, 0.1f};
    const float loud[] = {0.25f, -0.25f};
    pair.push(0, quiet, 2, 1, 48000); // mono, two frames
    pair.push(1, loud, 1, 2, 48000);  // stereo, a single frame
    const QByteArray out = pair.pull(3);
    const auto *pcm = reinterpret_cast<const qint16 *>(out.constData());
    check(out.size() == 12 && pcm[0] == qint16(std::lround(0.35f * 32767)) && pcm[1] == qint16(std::lround(-0.15f * 32767)),
          "audio mixer: sources are summed, both channels kept");
    check(pcm[2] == pcm[3] && pcm[2] == qint16(std::lround(0.1f * 32767)) && pcm[4] == 0 && pcm[5] == 0,
          "audio mixer: mono spreads to both channels; a short source adds silence");
}

void testMp4WithSound(const QDir &out)
{
    const QString path = out.filePath("m4_sound.mp4");
    QFile::remove(path);
    Mp4Encoder encoder;
    encoder.setAudio(true);
    bool ok = encoder.open(path, QSize(160, 120), 15);
    check(ok && encoder.hasAudio(), "mp4 sound: the AAC track is added");
    qint64 frame = 0;
    for (int ms = 0; ok && ms < 2000; ms += 10) {
        if (ms % 66 == 0)
            ok = encoder.addFrame(mp4Frame(ms / 66), ms);
        ok = ok && encoder.addAudio(sineChunk(frame, 480, 440), frame);
        frame += 480;
    }
    ok = ok && encoder.finish(2000);
    if (!ok)
        std::printf("      error: %s\n", qPrintable(encoder.errorString()));
    const auto [seconds, peak] = mp4Audio(path);
    std::printf("      sound %.2f s, peak %.2f\n", seconds, peak);
    check(ok && qAbs(seconds - 2.0) < 0.1 && peak > 0.2, "mp4 sound: 2 s of tone decode back");
}

// Click rings and shortcut captions, fed with synthetic events (no hooks are installed).
void testInputOverlay()
{
    check(InputOverlay::describeKey('C', true, false, false, false) == QStringLiteral("Ctrl + C"), "input: Ctrl+C shown");
    check(InputOverlay::describeKey('S', true, false, true, false) == QStringLiteral("Ctrl + Shift + S"),
          "input: modifiers listed in order");
    check(InputOverlay::describeKey('A', false, false, false, false).isEmpty()
              && InputOverlay::describeKey('A', false, false, true, false).isEmpty()
              && InputOverlay::describeKey(VK_SPACE, false, false, false, false).isEmpty(),
          "input: typing (letters, capitals, space) is never shown");
    check(InputOverlay::describeKey(VK_RETURN, false, false, false, false) == QStringLiteral("Enter")
              && InputOverlay::describeKey(VK_F5, false, false, false, false) == QStringLiteral("F5"),
          "input: Enter and F-keys shown on their own");
    check(InputOverlay::describeKey(VK_SHIFT, false, false, true, false).isEmpty(), "input: a modifier alone is not shown");

    InputOverlay overlay;
    const QPoint origin(100, 100);
    QImage frame(400, 300, QImage::Format_RGB32);
    frame.fill(Qt::white);
    QImage untouched = frame;
    overlay.paint(untouched, origin);
    check(untouched == frame, "input: nothing drawn without input");

    overlay.addPress({300, 250}, false);
    overlay.addKey('V', true, false, false, false);
    QImage drawn = frame;
    overlay.paint(drawn, origin);
    const QColor dot = drawn.pixelColor(200, 150);
    const QColor caption = drawn.pixelColor(200, 300 - 40);
    std::printf("      held click %s, caption background %s\n", qPrintable(dot.name()), qPrintable(caption.name()));
    check(dot.blue() > 200 && dot.red() < 230, "input: a held left button shows a blue dot");
    check(caption.lightness() < 120, "input: the shortcut caption sits at the bottom");

    overlay.addRelease(false);
    QTest::qWait(600); // past the ring and still within the caption
    QImage later = frame;
    overlay.paint(later, origin);
    check(later.pixelColor(200, 150) == Qt::white, "input: the click fades after release");
}

// ---- controllers end to end ------------------------------------------------------------------
// These read the real desktop's top-left 240x160 pixels (read-only; windows live on the offscreen
// platform, so nothing appears on screen) and delete what they write.

double mp4Seconds(const QString &path)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    double seconds = -1;
    ComPtr<IMFSourceReader> reader;
    const QString nativePath = QDir::toNativeSeparators(path);
    if (SUCCEEDED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, &reader))) {
        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)))
            seconds = duration.uhVal.QuadPart / 1e7;
        PropVariantClear(&duration);
    }
    reader.Reset();
    MFShutdown();
    return seconds;
}

// The panel moves out of the way: dragged by a non-button part, or collapsed to the time alone.
void testRecorderPanel()
{
    Recorder recorder(Recorder::Format::Gif, QGuiApplication::primaryScreen(), QRect(0, 0, 240, 160));
    QSignalSpy cancelled(&recorder, &Recorder::cancelled);
    recorder.start();
    QWidget *panel = nullptr;
    QPushButton *collapse = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->isVisible() && w->findChild<QPushButton *>("collapse")) {
            panel = w;
            collapse = w->findChild<QPushButton *>("collapse");
        }
    }
    check(panel != nullptr, "recorder panel: shown");
    if (panel) {
        const int fullWidth = panel->width();
        collapse->click();
        const bool narrower = panel->width() < fullWidth / 2;
        const QList<QPushButton *> buttons = panel->findChildren<QPushButton *>();
        const bool buttonsHidden = std::none_of(buttons.cbegin(), buttons.cend(), [collapse](QPushButton *b) {
            return b != collapse && b->isVisible();
        });
        check(narrower && buttonsHidden && Config::recordPanelCollapsed(),
              "recorder panel: collapsing leaves the time only, remembered");
        const int collapsedWidth = panel->width();
        collapse->click();
        const bool buttonsShown = std::all_of(buttons.cbegin(), buttons.cend(), [](QPushButton *b) { return b->isVisible(); });
        check(buttonsShown && panel->width() > collapsedWidth * 2 && !Config::recordPanelCollapsed(),
              "recorder panel: expands again");

        const QRect area = panel->screen()->availableGeometry();
        panel->move(area.center());
        const QPoint before = panel->pos();
        QTest::mousePress(panel, Qt::LeftButton, {}, QPoint(3, 3));
        QMouseEvent move(QEvent::MouseMove, QPointF(3, 3), QPointF(before + QPoint(3 - 60, 3 + 40)), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel, &move);
        QTest::mouseRelease(panel, Qt::LeftButton, {}, QPoint(3, 3));
        check(panel->pos() == before + QPoint(-60, 40), "recorder panel: dragged by its background");
    }
    recorder.cancel();
    cancelled.wait(15000);
}

void testRecorder(Recorder::Format format)
{
    const QByteArray name = Recorder::suffix(format).toUtf8();
    Recorder recorder(format, QGuiApplication::primaryScreen(), QRect(0, 0, 240, 160));
    QSignalSpy finished(&recorder, &Recorder::finished);
    QSignalSpy failed(&recorder, &Recorder::failed);
    // Measure what actually happened: under load qWait() can oversleep.
    recorder.start();
    QElapsedTimer wall; // from when recording runs (start() first sets up windows and the encoder)
    wall.start();
    QTest::qWait(1000);
    recorder.togglePause();
    const qint64 pausedAt = wall.elapsed();
    QTest::qWait(600); // paused time must not count
    recorder.togglePause();
    const qint64 paused = wall.elapsed() - pausedAt;
    QTest::qWait(500);
    recorder.stop();
    const double expected = (wall.elapsed() - paused) / 1000.0;
    finished.wait(15000);

    const QString path = finished.isEmpty() ? QString() : finished.first().first().toString();
    std::printf("      %s: %s\n", name.constData(),
                failed.isEmpty() ? qPrintable(QFileInfo(path).fileName()) : qPrintable(failed.first().first().toString()));
    check(finished.size() == 1 && failed.isEmpty() && QFileInfo::exists(path),
          (QByteArray("recorder ") + name + ": stop produces a file").constData());

    double seconds = -1;
    if (format == Recorder::Format::Gif) {
        QImageReader reader(path);
        int totalMs = 0, frames = 0;
        while (reader.canRead() && !reader.read().isNull()) {
            totalMs += reader.nextImageDelay();
            ++frames;
        }
        seconds = totalMs / 1000.0;
        std::printf("      %d frame(s), %.2f s\n", frames, seconds);
    } else {
        seconds = mp4Seconds(path);
        std::printf("      %.2f s, %lld bytes\n", seconds, QFileInfo(path).size());
        const auto [soundSeconds, peak] = mp4Audio(path);
        std::printf("      sound track %.2f s (system sound on by default), peak %.2f\n", soundSeconds, peak);
        check(qAbs(soundSeconds - seconds) < 0.3, "recorder mp4: the sound track lasts as long as the video");
    }
    std::printf("      expected %.2f s (recording time minus pause)\n", expected);
    check(qAbs(seconds - expected) < 0.2,
          (QByteArray("recorder ") + name + ": length is the recording time, pause excluded").constData());
    QFile::remove(path);
}

// When automatic scrolling continues or stops (pure logic; no input is sent).
void testAutoScroller()
{
    using D = AutoScroller::Decision;
    AutoScroller scroller;
    bool ok = scroller.next(true, false, false, false) == D::Scroll;
    // A page that animates: one idle tick, then content again, keeps going.
    ok = ok && scroller.next(false, false, false, false) == D::Scroll && scroller.next(true, false, false, false) == D::Scroll;
    check(ok, "autoscroll: keeps scrolling while new rows appear");

    D last = D::Scroll;
    for (int i = 0; i < AutoScroller::kIdleTicksAtEnd; ++i)
        last = scroller.next(false, false, false, false);
    check(last == D::StopAtEnd, "autoscroll: stops at the end after a few notches change nothing");

    scroller.reset();
    check(scroller.next(true, false, false, true) == D::StopByUser, "autoscroll: moving the mouse hands control back");
    check(scroller.next(false, true, false, false) == D::StopLost, "autoscroll: stops when a notch overshoots the region");
    check(scroller.next(true, false, true, false) == D::StopFull, "autoscroll: stops at the maximum height");
}

// Frames stop arriving early (the recorder dropped them under load), yet the video must last
// until the moment recording stopped.
void testMp4EndsAtStop(const QDir &out)
{
    const QString path = out.filePath("m4_gap.mp4");
    QFile::remove(path);
    Mp4Encoder encoder;
    bool ok = encoder.open(path, QSize(320, 240), 30);
    for (int i = 0; ok && i <= 15; ++i)
        ok = encoder.addFrame(mp4Frame(i), i * 33); // frames up to ~0.5 s
    ok = ok && encoder.finish(2000);                 // stopped at 2 s
    const double seconds = mp4Seconds(path);
    std::printf("      frames until 0.5 s, stopped at 2 s -> %.2f s\n", seconds);
    check(ok && qAbs(seconds - 2.0) < 0.1, "mp4: length runs until stop even when the last frames were dropped");
    QFile::remove(path);
}

// Backend choice: a small region stays on GDI under Auto; forcing WGC records through it.
void testRecorderBackends()
{
    Config::setCaptureBackend(Config::CaptureBackend::Auto);
    {
        Recorder recorder(Recorder::Format::Gif, QGuiApplication::primaryScreen(), QRect(0, 0, 240, 160));
        QSignalSpy finished(&recorder, &Recorder::finished);
        recorder.start();
        const QString backend = recorder.backendName();
        recorder.cancel();
        QSignalSpy cancelled(&recorder, &Recorder::cancelled);
        cancelled.wait(10000);
        std::printf("      auto, 240x160 -> %s\n", qPrintable(backend));
        check(WgcGrabber::canHideBorder() ? backend == "WGC" : backend == "GDI",
              "recorder: Auto keeps a small region on GDI on Windows 10 (no yellow border)");
    }
    if (!WgcGrabber::isSupported())
        return;

    Config::setCaptureBackend(Config::CaptureBackend::Wgc);
    Recorder recorder(Recorder::Format::Mp4, QGuiApplication::primaryScreen(), QRect(0, 0, 320, 240));
    QSignalSpy finished(&recorder, &Recorder::finished);
    recorder.start();
    const QString backend = recorder.backendName();
    QElapsedTimer wall;
    wall.start();
    QTest::qWait(1000);
    recorder.stop();
    const double expected = wall.elapsed() / 1000.0;
    finished.wait(15000);
    const QString path = finished.isEmpty() ? QString() : finished.first().first().toString();
    const double seconds = path.isEmpty() ? -1 : mp4Seconds(path);
    std::printf("      forced WGC -> %s, %.2f s\n", qPrintable(backend), seconds);
    std::printf("      expected about %.2f s\n", expected);
    check(backend == "WGC" && qAbs(seconds - expected) < 0.25, "recorder: forced WGC records a valid MP4");
    QFile::remove(path);
    Config::setCaptureBackend(Config::CaptureBackend::Auto);
}

void testRecorderCancel()
{
    Recorder recorder(Recorder::Format::Gif, QGuiApplication::primaryScreen(), QRect(0, 0, 240, 160));
    QSignalSpy cancelled(&recorder, &Recorder::cancelled);
    recorder.start();
    QTest::qWait(300);
    recorder.cancel();
    check(cancelled.wait(10000) && !QFileInfo::exists(recorder.filePath()), "recorder: cancel deletes the temp file");
}

void testScrollCapture()
{
    check(ScrollCapture::maxHeightFor(1920) == 30000 && ScrollCapture::maxHeightFor(3840) == 15625
              && ScrollCapture::maxHeightFor(8000) == 7500,
          "scroll capture: maximum length follows a pixel budget (4K-wide stops at 15625 px)");

    ScrollCapture capture(QGuiApplication::primaryScreen(), QRect(0, 0, 240, 160));
    QSignalSpy finished(&capture, &ScrollCapture::finished);
    capture.start();
    QTest::qWait(300);
    capture.finish(CaptureAction::Pin);
    const bool ok = finished.size() == 1 && finished.first().at(0).value<CaptureAction>() == CaptureAction::Pin
                    && finished.first().at(1).value<QPixmap>().size() == QSize(240, 160);
    check(ok, "scroll capture: finish delivers the region image with the chosen action");
}

// GDI vs Windows.Graphics.Capture on the full primary monitor (read-only; on Windows 10 the
// system shows a yellow border around the screen while WGC runs).
void testWgc()
{
    if (!WgcGrabber::isSupported()) {
        std::printf("      Windows.Graphics.Capture not supported: skipped\n");
        return;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    const QRect monitor(QPoint(info.rcMonitor.left, info.rcMonitor.top),
                        QPoint(info.rcMonitor.right - 1, info.rcMonitor.bottom - 1));

    constexpr int kGrabs = 30;
    QElapsedTimer timer;
    timer.start();
    QImage gdi;
    for (int i = 0; i < kGrabs; ++i)
        gdi = grabScreenRect(monitor, false);
    const double gdiMs = timer.nsecsElapsed() / 1e6 / kGrabs;

    WgcGrabber wgc;
    const bool started = wgc.start(monitor, false);
    check(started, "wgc: capture starts");
    if (!started) {
        std::printf("      %s\n", qPrintable(wgc.error()));
        return;
    }
    QImage first = wgc.grab(); // waits for the first frame
    timer.restart();
    QImage latest;
    for (int i = 0; i < kGrabs; ++i)
        latest = wgc.grab();
    const double wgcMs = timer.nsecsElapsed() / 1e6 / kGrabs;
    wgc.stop();

    std::printf("      %dx%d: GDI %.1f ms/frame, WGC %.1f ms/frame, border can be hidden: %s\n", monitor.width(),
                monitor.height(), gdiMs, wgcMs, WgcGrabber::canHideBorder() ? "yes" : "no (Windows 10)");
    check(first.size() == monitor.size() && latest.size() == monitor.size(), "wgc: frames cover the region");

    // Same screen through both paths, grabbed back to back: nearly all sampled pixels agree. The
    // desktop is live (someone may be working), so a few attempts are allowed.
    WgcGrabber again;
    int best = 0, samples = 0;
    if (again.start(monitor, false)) {
        for (int attempt = 0; attempt < 5 && best * 100 < samples * 95 + 1; ++attempt) {
            const QImage viaWgc = again.grab();
            const QImage viaGdi = grabScreenRect(monitor, false);
            int same = 0;
            samples = 0;
            for (int y = 5; y < monitor.height(); y += 37)
                for (int x = 5; x < monitor.width(); x += 41, ++samples)
                    same += colorsClose(viaGdi.pixel(x, y), viaWgc.pixel(x, y), 2);
            best = qMax(best, same);
            QTest::qWait(100);
        }
        again.stop();
    }
    std::printf("      %d of %d sampled pixels match GDI\n", best, samples);
    check(samples > 0 && best >= samples * 95 / 100, "wgc: frames show the same screen content as GDI");
}

} // namespace

void testTinyInputs(const QDir &out)
{
    ScrollStitcher stitcher(2000);
    QImage row(1, 1, QImage::Format_RGB32);
    row.fill(Qt::white);
    stitcher.add(row);
    row.fill(Qt::black);
    stitcher.add(row);
    check(!stitcher.result().isNull(), "tiny: 1x1 frames stitch without crashing");

    GifEncoder gif;
    bool ok = gif.open(out.filePath("m4_tiny.gif"), QSize(1, 1), 10) && gif.addFrame(row, 0) && gif.finish(100);
    check(ok && QImage(out.filePath("m4_tiny.gif")).size() == QSize(1, 1), "tiny: 1x1 GIF is written and readable");

    for (const QSize size : {QSize(1, 1), QSize(2, 2), QSize(3, 301), QSize(16, 16), QSize(33, 33)}) {
        const QString path = out.filePath("m4_tiny.mp4");
        QFile::remove(path);
        Mp4Encoder mp4;
        QImage frame(size, QImage::Format_RGB32);
        frame.fill(Qt::red);
        const bool opened = mp4.open(path, size, 15);
        const bool done = opened && mp4.addFrame(frame, 0) && mp4.addFrame(frame, 66) && mp4.finish(200);
        std::printf("      mp4 %dx%d: opened %d, finished %d, error '%s'\n", size.width(), size.height(), opened, done,
                    qPrintable(mp4.errorString()));
        check(done, "tiny: MP4 of a small region is padded to the encoder's minimum and written");
    }
}

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keeps the output up to a crash
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    QCoreApplication::addLibraryPath(QStringLiteral(JIEYING_QT_PLUGIN_DIR));
    QApplication app(argc, argv);
    QApplication::setOrganizationName("JieyingTests"); // recorder tests change settings
    QApplication::setApplicationName("m4_test");
    QSettings().clear();
    Config::setRecordClicks(false); // no global mouse hook while tests record the desktop
    const QDir out(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath());

    testStitcher(out);
    testSidewaysStitch(out);
    testSmoothScrolling(out);
    testGif(out);
    testMp4(out);
    testAudioMixer();
    testMp4WithSound(out);
    testRecorder(Recorder::Format::Gif);
    testRecorder(Recorder::Format::Mp4);
    testRecorderCancel();
    testAutoScroller();
    testInputOverlay();
    testRecorderPanel();
    testMp4EndsAtStop(out);
    testRecorderBackends();
    testScrollCapture();
    testWgc();
    testTinyInputs(out);

    QSettings().clear();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
