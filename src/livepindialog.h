#pragma once

#include "wgcgrabber.h"
#include "windowfinder.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>

#include <memory>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class LivePinPreview;

class LivePinSource
{
public:
    virtual ~LivePinSource() = default;

    virtual bool isSupported() = 0;
    virtual QList<CaptureWindow> windows() = 0;
    virtual CaptureWindowState windowState(const CaptureWindow &window) = 0;
    virtual bool start(const CaptureWindow &window) = 0;
    virtual WindowCaptureFrame poll() = 0;
    virtual void stop() = 0;
    virtual QString error() const = 0;
};

class LivePinDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit LivePinDialog(QWidget *parent = nullptr);
    LivePinDialog(std::unique_ptr<LivePinSource> source, QWidget *parent);

signals:
    // Full source-resolution pixels, preview image's global logical top-left,
    // and the scale that reproduces its visible size.
    void freezeRequested(const QPixmap &pixmap, const QPoint &contentTopLeft, qreal scale);

public slots:
    void done(int result) override;

private:
    void chooseWindow();
    void refreshWindows();
    void updateSelection();
    void beginCapture();
    void updateCapture();
    void resetFrame();
    void stopCapture();
    void finishCapture(const QString &message, const QString &detail = {});
    void freezeFrame();

    std::unique_ptr<LivePinSource> m_source;
    QStackedWidget *m_pages;
    QListWidget *m_windows;
    LivePinPreview *m_preview;
    QLabel *m_status;
    QPushButton *m_refresh;
    QPushButton *m_start;
    QPushButton *m_choose;
    QPushButton *m_freeze;
    QList<CaptureWindow> m_candidates;
    CaptureWindow m_target;
    QTimer m_timer;
    QElapsedTimer m_waiting;
    bool m_supported;
    bool m_capturing = false;
};
