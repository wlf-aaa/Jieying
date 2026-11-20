#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QString>

#include <mutex>
#include <thread>

// Draws mouse clicks and keyboard shortcuts into recorded frames, for tutorials. Input comes
// from low-level hooks installed only while recording with these options on. Only shortcuts
// (with Ctrl, Alt or Win) and non-character keys are shown, never typed text.
// The hooks run on a thread of their own: Windows waits for a low-level hook before moving the
// mouse on, and the recording thread is often busy grabbing the screen.
class InputOverlay
{
public:
    InputOverlay();
    ~InputOverlay();

    // Hooks the mouse (clicks) and/or keyboard (keys) until stop().
    void start(bool clicks, bool keys);
    void stop();

    // What the hooks report; public for tests. Positions are global physical pixels.
    void addPress(const QPoint &pos, bool right);
    void addRelease(bool right);
    void addKey(quint32 virtualKey, bool ctrl, bool alt, bool shift, bool win);

    // The caption for a key press, or empty when it is not shown (plain typing, modifiers alone).
    static QString describeKey(quint32 virtualKey, bool ctrl, bool alt, bool shift, bool win);

    // Draws what is current into a frame whose top-left is at origin (global physical pixels).
    void paint(QImage &frame, const QPoint &origin) const;
    qint64 now() const { return m_clock.elapsed(); }

private:
    struct Click
    {
        QPoint pos;
        qint64 pressedMs;
        qint64 releasedMs; // -1 while held
        bool right;
    };

    std::thread m_hookThread;
    unsigned long m_hookThreadId = 0;
    mutable std::mutex m_mutex; // the hook thread adds while the recording thread paints
    QElapsedTimer m_clock;
    QList<Click> m_clicks;
    QString m_caption;
    qint64 m_captionMs = -1;
};
