#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

class AudioCapture;
class FrameEncoder;
class InputOverlay;
class QLabel;
class QPushButton;
class QScreen;
class RegionFrame;
class WgcGrabber;

// Runs an encoder on its own thread; frames arrive as queued calls, in order.
class EncoderWorker : public QObject
{
    Q_OBJECT

public:
    explicit EncoderWorker(std::unique_ptr<FrameEncoder> encoder);
    ~EncoderWorker() override;

    void open(const QString &path, const QSize &size, int fps);
    void addFrame(const QImage &frame, qint64 timestampMs);
    void addAudio(const QByteArray &pcm, qint64 firstFrame);
    void finish(qint64 endMs);

    std::atomic<int> pending{0}; // frames queued but not yet encoded

signals:
    void failed(const QString &error);
    void finished(bool ok);

private:
    std::unique_ptr<FrameEncoder> m_encoder;
    bool m_ok = true;
};

// Records a screen region (cursor included) to GIF or MP4 in a temporary file, with a small
// control panel beside the region. The encoder streams to disk, so length is not bound by memory.
class Recorder : public QObject
{
    Q_OBJECT

public:
    enum class Format { Gif, Mp4 };

    // physicalRegion is in the screen's local physical pixels, as the overlay reports it.
    Recorder(Format format, QScreen *screen, const QRect &physicalRegion, QObject *parent = nullptr);
    ~Recorder() override;

    void start();
    Format format() const { return m_format; }
    QString filePath() const { return m_path; }
    static QString suffix(Format format) { return format == Format::Gif ? "gif" : "mp4"; }
    // "WGC" or "GDI": how frames are grabbed (chosen in start()).
    QString backendName() const { return m_wgc ? QStringLiteral("WGC") : QStringLiteral("GDI"); }
    // MP4 sound sources; they can be switched during the recording (the panel's buttons).
    void setSystemAudio(bool on);
    void setMicrophone(bool on);

public slots:
    void stop();
    void cancel();
    void togglePause();

signals:
    void finished(const QString &filePath); // the recording is complete on disk
    void failed(const QString &error);
    void cancelled();

private:
    void chooseBackend();
    QImage grabFrame();
    void captureFrame();
    qint64 elapsedMs() const;
    void buildPanel();
    void setPanelCollapsed(bool collapsed);
    void updateTime();
    void shutdown();

    Format m_format;
    QScreen *m_screen;
    QRect m_physical; // global physical pixels
    QRect m_logical;  // global logical pixels
    int m_fps;
    bool m_recordCursor;
    std::unique_ptr<WgcGrabber> m_wgc; // null: GDI
    std::unique_ptr<AudioCapture> m_audio; // MP4 only
    std::unique_ptr<InputOverlay> m_input; // clicks / shortcuts drawn into frames, when on
    QString m_path;

    QThread m_thread;
    EncoderWorker *m_worker = nullptr;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_pausedTotal = 0;
    qint64 m_pauseStart = -1;
    bool m_stopping = false;
    bool m_cancelled = false;
    QString m_error;

    QPointer<RegionFrame> m_frame;
    QPointer<QWidget> m_panel;
    QLabel *m_time = nullptr;
    QPushButton *m_pause = nullptr;
    QPushButton *m_speaker = nullptr;
    QPushButton *m_mic = nullptr;
    QPushButton *m_collapse = nullptr;
    QList<QWidget *> m_collapsible; // hidden when the panel is collapsed to the time alone
};
