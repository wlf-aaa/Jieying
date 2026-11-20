#include "recorder.h"

#include "audiocapture.h"
#include "config.h"
#include "frameencoder.h"
#include "icons.h"
#include "inputoverlay.h"
#include "regionframe.h"
#include "screengrab.h"
#include "wgcgrabber.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>

#include <optional>

namespace {
constexpr int kMaxPendingFrames = 12;           // beyond this the encoder is behind: drop frames
constexpr qint64 kMaxDurationMs = 10 * 60 * 1000; // safety stop

// Keeps a window on the screen it is over.
void keepOnScreen(QWidget *window)
{
    QScreen *screen = QGuiApplication::screenAt(window->geometry().center());
    if (!screen)
        screen = window->screen();
    const QRect area = screen->availableGeometry();
    const QSize size = window->size();
    window->move(qBound(area.left(), window->x(), qMax(area.left(), area.right() - size.width() + 1)),
                 qBound(area.top(), window->y(), qMax(area.top(), area.bottom() - size.height() + 1)));
}

// The recording panel; dragged by any part that is not a button, so it can be moved off what
// it covers.
class RecordPanel : public QWidget
{
public:
    RecordPanel()
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_grab = event->globalPosition().toPoint() - pos();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_grab && (event->buttons() & Qt::LeftButton))
            move(event->globalPosition().toPoint() - *m_grab);
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_grab)
            keepOnScreen(this);
        m_grab.reset();
    }

private:
    std::optional<QPoint> m_grab; // cursor position within the panel while dragging
};

} // namespace

EncoderWorker::EncoderWorker(std::unique_ptr<FrameEncoder> encoder)
    : m_encoder(std::move(encoder))
{
}

EncoderWorker::~EncoderWorker() = default;

void EncoderWorker::open(const QString &path, const QSize &size, int fps)
{
    m_ok = m_encoder->open(path, size, fps);
    if (!m_ok)
        emit failed(m_encoder->errorString());
}

void EncoderWorker::addFrame(const QImage &frame, qint64 timestampMs)
{
    --pending;
    if (m_ok && !m_encoder->addFrame(frame, timestampMs)) {
        m_ok = false;
        emit failed(m_encoder->errorString());
    }
}

void EncoderWorker::addAudio(const QByteArray &pcm, qint64 firstFrame)
{
    if (m_ok && !m_encoder->addAudio(pcm, firstFrame)) {
        m_ok = false;
        emit failed(m_encoder->errorString());
    }
}

void EncoderWorker::finish(qint64 endMs)
{
    if (m_ok)
        m_ok = m_encoder->finish(endMs);
    emit finished(m_ok);
}

Recorder::Recorder(Format format, QScreen *screen, const QRect &physicalRegion, QObject *parent)
    : QObject(parent)
    , m_format(format)
    , m_screen(screen)
    , m_fps(format == Format::Gif ? Config::gifFps() : Config::mp4Fps())
    , m_recordCursor(Config::recordCursor())
{
    const qreal dpr = screen->devicePixelRatio();
    const QPoint origin = screen->geometry().topLeft();
    m_physical = physicalRegion.translated(origin);
    m_logical = QRectF(physicalRegion.x() / dpr, physicalRegion.y() / dpr, physicalRegion.width() / dpr,
                       physicalRegion.height() / dpr)
                    .toAlignedRect()
                    .translated(origin);
    m_path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                 .filePath(QStringLiteral("jieying_%1.%2")
                               .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"), suffix(format)));

    std::unique_ptr<FrameEncoder> encoder;
    if (format == Format::Gif) {
        encoder = std::make_unique<GifEncoder>();
    } else {
        auto mp4 = std::make_unique<Mp4Encoder>();
        mp4->setBitsPerPixel(Config::bitsPerPixel(Config::mp4Quality()));
        mp4->setAudio(true); // always a track, so sound can be switched on mid-recording
        encoder = std::move(mp4);
    }
    m_worker = new EncoderWorker(std::move(encoder));
    if (format == Format::Mp4) {
        // Chunks come from the capture thread; queued onto the encoder's thread they stay in
        // order with the frames. stop() ends the capture before the finish call is queued.
        m_audio = std::make_unique<AudioCapture>(
            [w = m_worker](const QByteArray &pcm, qint64 firstFrame) {
                QMetaObject::invokeMethod(w, [w, pcm, firstFrame] { w->addAudio(pcm, firstFrame); },
                                          Qt::QueuedConnection);
            },
            [this](AudioCapture::Source source, const QString &error) {
                QMetaObject::invokeMethod(this, [this, source, error] {
                    qWarning("recording sound: %s", qPrintable(error));
                    QPushButton *button = source == AudioCapture::System ? m_speaker : m_mic;
                    if (button) {
                        const QSignalBlocker block(button);
                        button->setChecked(false);
                        button->setToolTip(error);
                    }
                }, Qt::QueuedConnection);
            });
        m_audio->setEnabled(AudioCapture::System, Config::recordSystemAudio());
        m_audio->setEnabled(AudioCapture::Microphone, Config::recordMicrophone());
    }
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &EncoderWorker::failed, this, [this](const QString &error) {
        m_error = error;
        stop();
    });
    connect(m_worker, &EncoderWorker::finished, this, [this](bool ok) {
        shutdown();
        if (m_cancelled) {
            QFile::remove(m_path);
            emit cancelled();
        } else if (ok && m_error.isEmpty()) {
            emit finished(m_path);
        } else {
            QFile::remove(m_path);
            emit failed(m_error.isEmpty() ? tr("编码失败") : m_error);
        }
    });

    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(1000 / m_fps);
    connect(&m_timer, &QTimer::timeout, this, &Recorder::captureFrame);
}

Recorder::~Recorder()
{
    m_timer.stop();
    if (m_audio)
        m_audio->stop();
    shutdown();
}

void Recorder::setSystemAudio(bool on)
{
    if (m_audio)
        m_audio->setEnabled(AudioCapture::System, on);
}

void Recorder::setMicrophone(bool on)
{
    if (m_audio)
        m_audio->setEnabled(AudioCapture::Microphone, on);
}

void Recorder::chooseBackend()
{
    const Config::CaptureBackend backend = Config::captureBackend();
    bool useWgc = backend == Config::CaptureBackend::Wgc;
    if (backend == Config::CaptureBackend::Auto && WgcGrabber::isSupported()) {
        if (WgcGrabber::canHideBorder()) {
            useWgc = true;
        } else {
            // Windows 10 shows a yellow border with WGC: worth it only when GDI cannot keep up.
            // The fastest of a few grabs, so one hiccup doesn't bring the border up for nothing.
            double fastestMs = 1e9;
            for (int i = 0; i < 3; ++i) {
                QElapsedTimer timer;
                timer.start();
                grabScreenRect(m_physical, m_recordCursor);
                fastestMs = qMin(fastestMs, timer.nsecsElapsed() / 1e6);
            }
            useWgc = fastestMs > 0.6 * 1000.0 / m_fps;
        }
    }
    if (useWgc) {
        m_wgc = std::make_unique<WgcGrabber>();
        if (!m_wgc->start(m_physical, m_recordCursor)) {
            qWarning("Windows.Graphics.Capture unavailable, recording with GDI: %s", qPrintable(m_wgc->error()));
            m_wgc.reset();
        }
    }
}

QImage Recorder::grabFrame()
{
    return m_wgc ? m_wgc->grab() : grabScreenRect(m_physical, m_recordCursor);
}

void Recorder::start()
{
    chooseBackend();
    m_thread.start();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, path = m_path, size = m_physical.size(), fps = m_fps] {
        w->open(path, size, fps);
    }, Qt::QueuedConnection);

    m_frame = new RegionFrame(m_screen, m_logical, QColor(230, 40, 40));
    m_frame->show();
    buildPanel();
    m_clock.start();
    if (m_audio)
        m_audio->start();
    if (Config::recordClicks() || Config::recordKeys()) {
        m_input = std::make_unique<InputOverlay>();
        m_input->start(Config::recordClicks(), Config::recordKeys());
    }
    captureFrame();
    m_timer.start();
}

void Recorder::buildPanel()
{
    auto *panel = new RecordPanel;
    panel->setObjectName("panel");
    panel->setAttribute(Qt::WA_StyledBackground);
    panel->setAttribute(Qt::WA_DeleteOnClose);
    panel->setAttribute(Qt::WA_ShowWithoutActivating);
    panel->setStyleSheet(kPanelStyle);

    auto *grip = new QLabel(panel);
    grip->setPixmap(toolIcon("grip").pixmap(QSize(12, 16)));
    grip->setCursor(Qt::SizeAllCursor);
    grip->setToolTip(tr("拖动可移开控制条"));
    auto *dot = new QLabel(QStringLiteral("●"), panel);
    dot->setStyleSheet("color: #e53935;");
    m_time = new QLabel(panel);
    m_time->setMinimumWidth(m_time->fontMetrics().horizontalAdvance("00:00:00"));
    auto *format = new QLabel(suffix(m_format).toUpper(), panel);
    format->setStyleSheet("color: #888;");
    m_pause = new QPushButton(toolIcon("pause"), tr("暂停"), panel);
    connect(m_pause, &QPushButton::clicked, this, &Recorder::togglePause);
    auto *stop = new QPushButton(toolIcon("stop"), tr("停止"), panel);
    stop->setToolTip(tr("也可以再按一次截图快捷键"));
    connect(stop, &QPushButton::clicked, this, &Recorder::stop);
    auto *cancel = new QPushButton(toolIcon("cancel"), tr("取消"), panel);
    connect(cancel, &QPushButton::clicked, this, &Recorder::cancel);
    if (m_audio) {
        // Sound switches, remembered for the next recording.
        auto soundButton = [panel](const char *icon, const QString &tip, bool on) {
            auto *button = new QPushButton(toolIcon(icon), QString(), panel);
            button->setCheckable(true);
            button->setChecked(on);
            button->setToolTip(tip);
            return button;
        };
        m_speaker = soundButton("speaker", tr("录制系统声音（点击开关）"), m_audio->isEnabled(AudioCapture::System));
        connect(m_speaker, &QPushButton::toggled, this, [this](bool on) {
            setSystemAudio(on);
            Config::setRecordSystemAudio(on);
        });
        m_mic = soundButton("mic", tr("录制麦克风（点击开关）"), m_audio->isEnabled(AudioCapture::Microphone));
        connect(m_mic, &QPushButton::toggled, this, [this](bool on) {
            setMicrophone(on);
            Config::setRecordMicrophone(on);
        });
    }

    m_collapse = new QPushButton(panel);
    m_collapse->setObjectName("collapse");
    connect(m_collapse, &QPushButton::clicked, this, [this] { setPanelCollapsed(!m_collapsible.first()->isHidden()); });

    auto *layout = new QHBoxLayout(panel);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    for (QWidget *w : {static_cast<QWidget *>(grip), static_cast<QWidget *>(dot), static_cast<QWidget *>(m_time)})
        layout->addWidget(w);
    m_collapsible = {format};
    if (m_speaker)
        m_collapsible << m_speaker << m_mic;
    m_collapsible << m_pause << stop << cancel;
    for (QWidget *w : std::as_const(m_collapsible))
        layout->addWidget(w);
    layout->addWidget(m_collapse);

    m_panel = panel;
    updateTime();
    setPanelCollapsed(Config::recordPanelCollapsed());
    panel->move(placeBeside(m_screen, m_logical, panel->size()));
    panel->show();
    excludeFromCapture(panel);
}

void Recorder::setPanelCollapsed(bool collapsed)
{
    for (QWidget *w : std::as_const(m_collapsible))
        w->setHidden(collapsed);
    m_collapse->setIcon(toolIcon(collapsed ? "expand" : "collapse"));
    m_collapse->setToolTip(collapsed ? tr("展开控制条") : tr("收起控制条，只显示录制时间（停止也可以再按一次截图快捷键）"));
    Config::setRecordPanelCollapsed(collapsed);
    m_panel->ensurePolished(); // the style sheet's padding counts before the first show too
    m_panel->layout()->activate();
    m_panel->adjustSize();
    if (m_panel->isVisible())
        keepOnScreen(m_panel);
}

qint64 Recorder::elapsedMs() const
{
    const qint64 now = m_clock.elapsed();
    return now - m_pausedTotal - (m_pauseStart >= 0 ? now - m_pauseStart : 0);
}

void Recorder::updateTime()
{
    if (!m_time)
        return;
    const qint64 seconds = elapsedMs() / 1000;
    m_time->setText(QStringLiteral("%1:%2")
                        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
                        .arg(seconds % 60, 2, 10, QLatin1Char('0')));
}

void Recorder::captureFrame()
{
    if (m_pauseStart >= 0 || m_stopping)
        return;
    const qint64 ms = elapsedMs();
    if (ms >= kMaxDurationMs) {
        stop();
        return;
    }
    updateTime();
    if (m_worker->pending.load() > kMaxPendingFrames)
        return; // the encoder catches up; the previous frame simply lasts longer
    QImage frame = grabFrame();
    if (frame.isNull())
        return; // capture has not delivered its first frame yet
    if (m_input)
        m_input->paint(frame, m_physical.topLeft());
    ++m_worker->pending;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, frame, ms] { w->addFrame(frame, ms); }, Qt::QueuedConnection);
}

void Recorder::togglePause()
{
    if (m_stopping)
        return;
    if (m_audio)
        m_audio->setPaused(m_pauseStart < 0);
    if (m_pauseStart >= 0) {
        m_pausedTotal += m_clock.elapsed() - m_pauseStart;
        m_pauseStart = -1;
        m_pause->setText(tr("暂停"));
        m_pause->setIcon(toolIcon("pause"));
    } else {
        m_pauseStart = m_clock.elapsed();
        m_pause->setText(tr("继续"));
        m_pause->setIcon(toolIcon("resume"));
    }
}

void Recorder::stop()
{
    if (m_stopping)
        return;
    if (m_pauseStart >= 0)
        togglePause(); // close the pause interval so the end time excludes it
    m_stopping = true;
    m_timer.stop();
    const qint64 end = elapsedMs();
    if (m_audio)
        m_audio->stop(); // its last chunk is queued before the finish call
    if (m_panel)
        m_panel->setEnabled(false);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, end] { w->finish(end); }, Qt::QueuedConnection);
}

void Recorder::cancel()
{
    m_cancelled = true;
    stop();
}

void Recorder::shutdown()
{
    m_input.reset(); // unhooks the mouse and keyboard
    if (m_wgc)
        m_wgc->stop(); // ends the capture session (and Windows 10's yellow border) right away
    if (m_frame)
        m_frame->close();
    if (m_panel)
        m_panel->close();
    if (m_thread.isRunning()) {
        m_thread.quit();
        m_thread.wait();
    }
}
