#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Mixes audio sources that deliver at their own pace and sample rate into one 48 kHz stereo
// 16-bit stream. Pure logic, exposed for tests.
class AudioMixer
{
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;

    explicit AudioMixer(int sources);

    // A device packet: frames of `channels` interleaved samples at `rate`. Mono is spread to
    // both sides; channels past the first two are dropped. Resampled (linear) to 48 kHz.
    void push(int source, const float *data, int frames, int channels, int rate);
    // Forgets a source's queued audio and resampler state (its device was closed).
    void reset(int source);
    int queuedFrames(int source) const { return int(m_sources[source].fifo.size() / kChannels); }

    // The next `frames` output frames as interleaved int16. A source that has less contributes
    // silence for the rest (loopback delivers nothing while nothing plays); one far ahead loses
    // its oldest audio so it cannot drift out of sync.
    QByteArray pull(int frames);

private:
    struct Source
    {
        std::deque<float> fifo; // interleaved stereo at 48 kHz
        double position = 0;    // next output's position in the current input, in input frames
        float previous[kChannels] = {};
    };
    std::vector<Source> m_sources;
};

// Records the system sound (WASAPI loopback) and/or the microphone on its own thread and hands
// out mixed 48 kHz stereo PCM chunks, continuous in time: silence fills the gaps, and paused
// time is skipped, matching the recorder's clock. A source's device is open only while that
// source is on, so the microphone is not held (nor shown as in use) when off.
class AudioCapture
{
public:
    enum Source { System = 0, Microphone = 1 };

    // chunk(pcm, firstFrame): interleaved int16 stereo, firstFrame counted from the start at
    // 48 kHz. Called on the capture thread.
    using ChunkHandler = std::function<void(const QByteArray &pcm, qint64 firstFrame)>;
    // A device could not be opened; that source is switched off. Called on the capture thread.
    using FailureHandler = std::function<void(Source source, const QString &error)>;

    AudioCapture(ChunkHandler chunk, FailureHandler failure);
    ~AudioCapture();

    void setEnabled(Source source, bool on);
    bool isEnabled(Source source) const { return m_enabled[source].load(); }

    void start();
    void setPaused(bool paused);
    // Delivers the audio up to now and ends the thread; no handler runs after this returns.
    void stop();

private:
    void run();
    qint64 clockFrames(); // 48 kHz frames recorded so far, pauses excluded

    ChunkHandler m_chunk;
    FailureHandler m_failure;
    std::atomic<bool> m_enabled[2] = {false, false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;

    std::mutex m_clockMutex;
    std::chrono::steady_clock::time_point m_started;
    std::chrono::steady_clock::duration m_pausedTotal{};
    std::chrono::steady_clock::time_point m_pauseStart;
    bool m_paused = false;
};
