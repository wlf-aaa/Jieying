#include "audiocapture.h"

#include <QtGlobal>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cmath>

using Microsoft::WRL::ComPtr;

// ---- AudioMixer -------------------------------------------------------------------------------

AudioMixer::AudioMixer(int sources)
    : m_sources(size_t(sources))
{
}

void AudioMixer::push(int source, const float *data, int frames, int channels, int rate)
{
    if (frames <= 0 || channels <= 0 || rate <= 0)
        return;
    Source &s = m_sources[size_t(source)];
    auto sample = [&](int frame, int side) -> float {
        if (frame < 0)
            return s.previous[side];
        return data[size_t(frame) * channels + (channels > 1 ? side : 0)];
    };
    // Linear interpolation; position -1 is the previous packet's last frame.
    const double step = double(rate) / kRate;
    while (s.position <= frames - 1) {
        const int i0 = int(std::floor(s.position));
        const float f = float(s.position - i0);
        for (int side = 0; side < kChannels; ++side) {
            const float a = sample(i0, side);
            const float b = i0 + 1 <= frames - 1 ? sample(i0 + 1, side) : a;
            s.fifo.push_back(a + (b - a) * f);
        }
        s.position += step;
    }
    s.position -= frames;
    for (int side = 0; side < kChannels; ++side)
        s.previous[side] = sample(frames - 1, side);
}

void AudioMixer::reset(int source)
{
    m_sources[size_t(source)] = Source();
}

QByteArray AudioMixer::pull(int frames)
{
    constexpr int kMaxLeadFrames = kRate / 5; // 200 ms ahead of the clock: drop the excess
    QByteArray out(qsizetype(frames) * kChannels * 2, Qt::Uninitialized);
    auto *pcm = reinterpret_cast<qint16 *>(out.data());
    std::vector<float> mix(size_t(frames) * kChannels, 0.f);
    for (Source &s : m_sources) {
        const size_t queued = s.fifo.size() / kChannels;
        if (queued > size_t(frames + kMaxLeadFrames))
            s.fifo.erase(s.fifo.begin(), s.fifo.begin() + std::ptrdiff_t((queued - frames) * kChannels));
        const size_t take = qMin(s.fifo.size(), mix.size());
        for (size_t i = 0; i < take; ++i)
            mix[i] += s.fifo[i];
        s.fifo.erase(s.fifo.begin(), s.fifo.begin() + std::ptrdiff_t(take));
    }
    for (size_t i = 0; i < mix.size(); ++i)
        pcm[i] = qint16(qBound(-32768, int(std::lround(mix[i] * 32767.f)), 32767));
    return out;
}

// ---- WASAPI devices ---------------------------------------------------------------------------

namespace {

struct Device
{
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    int channels = 0;
    int rate = 0;
    int bits = 0;
    bool isFloat = false;
    std::vector<float> scratch;

    void close()
    {
        if (client)
            client->Stop();
        capture.Reset();
        client.Reset();
    }
};

QString hresultText(const char *step, HRESULT hr)
{
    return QStringLiteral("%1 (0x%2)").arg(QLatin1String(step)).arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

// Opens the default output (loopback: what the speakers play) or the default microphone.
QString openDevice(Device &device, bool loopback)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
        return hresultText("MMDeviceEnumerator", hr);
    ComPtr<IMMDevice> endpoint;
    hr = enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &endpoint);
    if (FAILED(hr))
        return loopback ? QStringLiteral("没有可用的播放设备") : QStringLiteral("没有可用的麦克风");
    hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(device.client.GetAddressOf()));
    if (FAILED(hr))
        return hresultText("IAudioClient", hr);

    WAVEFORMATEX *format = nullptr;
    hr = device.client->GetMixFormat(&format);
    if (FAILED(hr))
        return hresultText("GetMixFormat", hr);
    device.channels = format->nChannels;
    device.rate = int(format->nSamplesPerSec);
    device.bits = format->wBitsPerSample;
    device.isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        device.isFloat = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    constexpr REFERENCE_TIME kBuffer = 10'000'000; // 1 s, plenty for a 10 ms polling loop
    hr = device.client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, kBuffer, 0,
                                   format, nullptr);
    CoTaskMemFree(format);
    if (FAILED(hr))
        return hresultText("IAudioClient::Initialize", hr);
    hr = device.client->GetService(IID_PPV_ARGS(&device.capture));
    if (SUCCEEDED(hr))
        hr = device.client->Start();
    if (FAILED(hr))
        return hresultText("IAudioClient::Start", hr);
    if (!device.isFloat && device.bits != 16 && device.bits != 24 && device.bits != 32)
        return QStringLiteral("unsupported sample format (%1 bit)").arg(device.bits);
    return {};
}

// Converts one packet to float samples in device.scratch.
const float *toFloat(Device &device, const BYTE *data, UINT32 frames)
{
    const size_t count = size_t(frames) * device.channels;
    if (device.isFloat)
        return reinterpret_cast<const float *>(data);
    device.scratch.resize(count);
    for (size_t i = 0; i < count; ++i) {
        switch (device.bits) {
        case 16:
            device.scratch[i] = reinterpret_cast<const qint16 *>(data)[i] / 32768.f;
            break;
        case 24: {
            const BYTE *p = data + i * 3;
            const qint32 v = qint32((quint32(p[0]) << 8) | (quint32(p[1]) << 16) | (quint32(p[2]) << 24)) >> 8;
            device.scratch[i] = v / 8388608.f;
            break;
        }
        default:
            device.scratch[i] = float(reinterpret_cast<const qint32 *>(data)[i] / 2147483648.0);
        }
    }
    return device.scratch.data();
}

} // namespace

// ---- AudioCapture -----------------------------------------------------------------------------

AudioCapture::AudioCapture(ChunkHandler chunk, FailureHandler failure)
    : m_chunk(std::move(chunk))
    , m_failure(std::move(failure))
{
}

AudioCapture::~AudioCapture()
{
    stop();
}

void AudioCapture::setEnabled(Source source, bool on)
{
    m_enabled[source] = on;
}

void AudioCapture::start()
{
    if (m_thread.joinable())
        return;
    m_started = std::chrono::steady_clock::now();
    m_stop = false;
    m_thread = std::thread(&AudioCapture::run, this);
}

void AudioCapture::setPaused(bool paused)
{
    const std::lock_guard lock(m_clockMutex);
    if (paused == m_paused)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (paused)
        m_pauseStart = now;
    else
        m_pausedTotal += now - m_pauseStart;
    m_paused = paused;
}

void AudioCapture::stop()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

qint64 AudioCapture::clockFrames()
{
    const std::lock_guard lock(m_clockMutex);
    const auto end = m_paused ? m_pauseStart : std::chrono::steady_clock::now();
    const auto recorded = end - m_started - m_pausedTotal;
    return std::chrono::duration_cast<std::chrono::microseconds>(recorded).count() * AudioMixer::kRate / 1'000'000;
}

void AudioCapture::run()
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    AudioMixer mixer(2);
    Device devices[2];
    qint64 delivered = 0;

    for (bool last = false; !last;) {
        last = m_stop.load(); // one more round after stop() to deliver the tail
        bool paused;
        {
            const std::lock_guard lock(m_clockMutex);
            paused = m_paused;
        }
        for (int i = 0; i < 2; ++i) {
            Device &device = devices[i];
            const bool want = m_enabled[i].load() && !last;
            if (want && !device.client) {
                const QString error = openDevice(device, i == System);
                if (!error.isEmpty()) {
                    device.close();
                    m_enabled[i] = false;
                    if (m_failure)
                        m_failure(Source(i), error);
                    continue;
                }
            } else if (!want && device.client) {
                device.close();
                mixer.reset(i);
            }
            if (!device.capture)
                continue;
            UINT32 packet = 0;
            while (SUCCEEDED(device.capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(device.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;
                if (!paused) {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        const std::vector<float> silence(size_t(frames) * device.channels, 0.f);
                        mixer.push(i, silence.data(), int(frames), device.channels, device.rate);
                    } else {
                        mixer.push(i, toFloat(device, data, frames), int(frames), device.channels, device.rate);
                    }
                }
                device.capture->ReleaseBuffer(frames);
            }
        }

        const qint64 target = clockFrames();
        const qint64 due = target - delivered;
        if (due >= AudioMixer::kRate / 100 || (last && due > 0)) { // 10 ms chunks, plus the tail
            if (m_chunk)
                m_chunk(mixer.pull(int(due)), delivered);
            delivered = target;
        }
        if (!last)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (Device &device : devices)
        device.close();
    if (com)
        CoUninitialize();
}
