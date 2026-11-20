#include "frameencoder.h"

#include <QDir>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct Mp4Encoder::Private
{
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    bool wantAudio = false;
    bool audio = false; // the sound track was added
    DWORD audioStream = 0;
    QSize size;
    qint64 lastMs = -1;
    QImage pending; // held until the next frame says how long it lasts
    double bitsPerPixel = 0.15; // screen content compresses well; this keeps text crisp
    bool comInitialized = false;
    bool mfStarted = false;
};

namespace {

QString hresultText(const char *step, HRESULT hr)
{
    return QStringLiteral("%1 failed (0x%2)").arg(QLatin1String(step)).arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

HRESULT setVideoType(IMFMediaType *type, const GUID &subtype, const QSize &size, int fps)
{
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, UINT32(size.width()), UINT32(size.height()));
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, UINT32(fps), 1);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return hr;
}

constexpr UINT32 kAudioRate = 48000;
constexpr UINT32 kAudioChannels = 2;

HRESULT setAudioType(IMFMediaType *type, const GUID &subtype, UINT32 bytesPerSecond)
{
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
    if (SUCCEEDED(hr) && subtype == MFAudioFormat_PCM)
        hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kAudioChannels * 2);
    return hr;
}

} // namespace

Mp4Encoder::Mp4Encoder()
    : d(new Private)
{
}

void Mp4Encoder::setBitsPerPixel(double bitsPerPixel)
{
    d->bitsPerPixel = bitsPerPixel;
}

void Mp4Encoder::setAudio(bool on)
{
    d->wantAudio = on;
}

bool Mp4Encoder::hasAudio() const
{
    return d->audio;
}

Mp4Encoder::~Mp4Encoder()
{
    release();
    delete d;
}

void Mp4Encoder::release()
{
    d->writer.Reset();
    if (d->mfStarted)
        MFShutdown();
    if (d->comInitialized)
        CoUninitialize();
    d->mfStarted = d->comInitialized = false;
}

bool Mp4Encoder::open(const QString &path, const QSize &requested, int fps)
{
    // H.264 needs even dimensions, and Windows' encoders refuse anything under 32 px; a smaller
    // region is padded (frames are copied into the top-left corner, the rest stays black).
    constexpr int kMinSide = 32;
    d->size = QSize(qMax(kMinSide, requested.width() & ~1), qMax(kMinSide, requested.height() & ~1));
    if (requested.isEmpty())
        return fail(QStringLiteral("region too small for video"));

    d->comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
        return fail(hresultText("MFStartup", hr));
    d->mfStarted = true;

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    const QString nativePath = QDir::toNativeSeparators(path);
    hr = MFCreateSinkWriterFromURL(reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, attributes.Get(),
                                   &d->writer);
    if (FAILED(hr))
        return fail(hresultText("MFCreateSinkWriterFromURL", hr));

    ComPtr<IMFMediaType> output;
    MFCreateMediaType(&output);
    hr = setVideoType(output.Get(), MFVideoFormat_H264, d->size, fps);
    const UINT32 bitrate =
        UINT32(qBound(500'000.0, d->size.width() * d->size.height() * fps * d->bitsPerPixel, 60'000'000.0));
    if (SUCCEEDED(hr))
        hr = output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr))
        hr = d->writer->AddStream(output.Get(), &d->stream);
    if (FAILED(hr))
        return fail(hresultText("AddStream", hr));

    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    hr = setVideoType(input.Get(), MFVideoFormat_RGB32, d->size, fps);
    if (SUCCEEDED(hr)) // positive stride: rows are top-down, as in QImage
        hr = input->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(d->size.width() * 4));
    if (SUCCEEDED(hr))
        hr = d->writer->SetInputMediaType(d->stream, input.Get(), nullptr);
    if (FAILED(hr))
        return fail(hresultText("SetInputMediaType", hr));

    d->audio = false;
    if (d->wantAudio) {
        ComPtr<IMFMediaType> aac, pcm;
        MFCreateMediaType(&aac);
        MFCreateMediaType(&pcm);
        HRESULT audioHr = setAudioType(aac.Get(), MFAudioFormat_AAC, 16'000); // 128 kbit/s
        if (SUCCEEDED(audioHr))
            audioHr = d->writer->AddStream(aac.Get(), &d->audioStream);
        if (SUCCEEDED(audioHr))
            audioHr = setAudioType(pcm.Get(), MFAudioFormat_PCM, kAudioRate * kAudioChannels * 2);
        if (SUCCEEDED(audioHr))
            audioHr = d->writer->SetInputMediaType(d->audioStream, pcm.Get(), nullptr);
        d->audio = SUCCEEDED(audioHr);
        if (!d->audio)
            qWarning("MP4 without sound: %s", qPrintable(hresultText("audio stream", audioHr)));
    }

    hr = d->writer->BeginWriting();
    if (FAILED(hr))
        return fail(hresultText("BeginWriting", hr));
    d->lastMs = -1;
    return true;
}

bool Mp4Encoder::addFrame(const QImage &input, qint64 timestampMs)
{
    if (!d->writer)
        return fail(QStringLiteral("encoder not open"));
    if (input.isNull() || timestampMs <= d->lastMs)
        return true; // the container needs strictly increasing timestamps
    // Each frame is written once the next one arrives, so it lasts exactly until then even when
    // the recorder dropped frames in between.
    if (!d->pending.isNull() && !writeSample(d->pending, d->lastMs, timestampMs - d->lastMs))
        return false;
    d->pending = input.convertToFormat(QImage::Format_RGB32).copy(QRect(QPoint(0, 0), d->size));
    d->lastMs = timestampMs;
    return true;
}

bool Mp4Encoder::writeSample(const QImage &frame, qint64 timestampMs, qint64 durationMs)
{
    const DWORD stride = DWORD(d->size.width() * 4);
    const DWORD bytes = stride * DWORD(d->size.height());

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
    BYTE *data = nullptr;
    if (SUCCEEDED(hr))
        hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        MFCopyImage(data, LONG(stride), frame.constBits(), LONG(frame.bytesPerLine()), stride, DWORD(d->size.height()));
        buffer->Unlock();
        hr = buffer->SetCurrentLength(bytes);
    }
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr))
        hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr))
        hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr))
        hr = sample->SetSampleTime(timestampMs * 10'000); // 100 ns units
    if (SUCCEEDED(hr))
        hr = sample->SetSampleDuration(qMax<qint64>(1, durationMs) * 10'000);
    if (SUCCEEDED(hr))
        hr = d->writer->WriteSample(d->stream, sample.Get());
    if (FAILED(hr))
        return fail(hresultText("WriteSample", hr));
    return true;
}

bool Mp4Encoder::addAudio(const QByteArray &pcm, qint64 firstFrame)
{
    if (!d->writer)
        return fail(QStringLiteral("encoder not open"));
    if (!d->audio || pcm.isEmpty())
        return true;
    const auto bytes = DWORD(pcm.size());
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
    BYTE *data = nullptr;
    if (SUCCEEDED(hr))
        hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(data, pcm.constData(), bytes);
        buffer->Unlock();
        hr = buffer->SetCurrentLength(bytes);
    }
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr))
        hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr))
        hr = sample->AddBuffer(buffer.Get());
    const qint64 frames = pcm.size() / (kAudioChannels * 2);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleTime(firstFrame * 10'000'000 / kAudioRate);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleDuration(frames * 10'000'000 / kAudioRate);
    if (SUCCEEDED(hr))
        hr = d->writer->WriteSample(d->audioStream, sample.Get());
    if (FAILED(hr))
        return fail(hresultText("WriteSample (audio)", hr));
    return true;
}

bool Mp4Encoder::finish(qint64 endMs)
{
    if (!d->writer)
        return fail(QStringLiteral("encoder not open"));
    // The last frame stays up until the recording stopped.
    if (!d->pending.isNull() && !writeSample(d->pending, d->lastMs, endMs - d->lastMs))
        return false;
    d->pending = QImage();
    const HRESULT hr = d->writer->Finalize();
    release();
    return SUCCEEDED(hr) || fail(hresultText("Finalize", hr));
}
