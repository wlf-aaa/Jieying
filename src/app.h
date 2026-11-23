#pragma once

#include "capturetoolbar.h"
#include "config.h"
#include "screengrab.h"

#include <QList>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QStringList>
#include <optional>

class CaptureHistory;
class CaptureSession;
class CollageDialog;
class HistoryDialog;
class HotkeyManager;
class LivePinDialog;
class PinBoard;
class PinWindow;
class QAction;
class QMenu;
class QScreen;
class QSystemTrayIcon;
class QTimer;
class Recorder;
class ScrollCapture;
class SelectionOverlay;
class SettingsDialog;

// Tray-resident controller: owns hotkeys, the active capture session and pinned images.
class App : public QObject
{
    Q_OBJECT

public:
    explicit App(QObject *parent = nullptr);

    bool init();

    // A request forwarded by a second launch: "capture", or "activate" (plain double-click).
    void handleInstanceMessage(const QString &message);

public slots:
    // With an autoAction (long screenshot, recording) the selection runs it immediately.
    void startCapture(std::optional<CaptureAction> autoAction = std::nullopt, const QRect &preselect = QRect());
    void captureScreenUnderCursor();
    void captureActiveWindow();
    void pinClipboard();
    void closeAllPins();
    void restorePinClicks();
    void togglePins();
    void showSettings();

private:
    void createTray();
    void updateTrayTexts();
    void onHotkey(Config::Hotkey hotkey);
    void repeatCapture();
    void configureFullscreenHotkeyPause(bool enabled);
    void addPin(const QPixmap &image, const QPoint &contentTopLeft, qreal initialScale = 1.0);
    void connectPinActions(PinWindow *pin);
    void restoreLastPin();
    // Pins an image at the cursor, pixels 1:1 on the screen under it.
    void pinAtCursor(const QImage &image);
    // Several images side by side from pos (logical, top-left of the first), 1:1 on that screen.
    void pinImagesAt(const QList<QImage> &images, const QPoint &pos);
    void fillHistoryMenu(QMenu *menu);
    void showHistory();
    void showCollage(const QStringList &paths = {});
    void showLivePin();
    void startDelayedCapture(int seconds);
    // Opens the annotation editor over a pinned image; "done" puts the result back on the pin.
    void annotatePin(PinWindow *pin);
    void showOcr(const QPixmap &image);
    void showBeautify(const QPixmap &image);
    void deliver(CaptureAction action, const QPixmap &image, const QPoint &globalTopLeft);
    void startRegionMode(CaptureAction action, QScreen *screen, const QRect &physicalRegion);
    void saveRecording(const QString &path, const QString &suffix);
    void notify(const QString &message);

    HotkeyManager *m_hotkeys = nullptr;
    QTimer *m_fullscreenPoll = nullptr;
    CaptureHistory *m_history = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_captureAction = nullptr;
    QAction *m_repeatCaptureAction = nullptr;
    QAction *m_pinAction = nullptr;
    QAction *m_togglePinsAction = nullptr;
    QAction *m_restoreClicksAction = nullptr;
    QPointer<CaptureSession> m_session;
    QPointer<QScreen> m_lastCaptureScreen;
    ScreenshotArea m_lastCaptureArea;
    QPointer<ScrollCapture> m_scrollCapture;
    QPointer<Recorder> m_recorder;
    QPointer<SettingsDialog> m_settings;
    QPointer<HistoryDialog> m_historyDialog;
    QPointer<CollageDialog> m_collageDialog;
    QPointer<LivePinDialog> m_livePinDialog;
    QPointer<SelectionOverlay> m_pinEditor;
    PinBoard *m_board = nullptr;
};
