#include "app.h"

#include "beautifydialog.h"
#include "capturehistory.h"
#include "capturesession.h"
#include "collagedialog.h"
#include "historydialog.h"
#include "hotkeymanager.h"
#include "imageio.h"
#include "livepindialog.h"
#include "ocr.h"
#include "ocrwindow.h"
#include "pinboard.h"
#include "pinwindow.h"
#include "recorder.h"
#include "scrollcapture.h"
#include "selectionoverlay.h"
#include "settingsdialog.h"
#include "windowfinder.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

namespace {

// Delay so the tray menu or the closing overlays are gone before the screen is grabbed.
constexpr int kMenuCloseDelayMs = 200;

// assets/jieying.ico (tools/make_icon.py), embedded as a Qt resource; also the exe icon.
QIcon makeAppIcon()
{
    return QIcon(QStringLiteral(":/jieying.ico"));
}

QString keyText(Config::Hotkey hotkey)
{
    return Config::hotkey(hotkey).toString(QKeySequence::NativeText);
}

} // namespace

App::App(QObject *parent)
    : QObject(parent)
{
}

bool App::init()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return false;
    QApplication::setWindowIcon(makeAppIcon());

    m_history = new CaptureHistory(CaptureHistory::defaultFolder(), this);
    m_history->prune(); // the configured size may have shrunk since last time
    m_board = new PinBoard(this);
    connect(m_board, &PinBoard::groupShown, this, [](int group, int pins) {
        QToolTip::showText(QCursor::pos(), pins > 0 ? tr("贴图分组 %1 · %2 张").arg(group).arg(pins)
                                                    : tr("贴图分组 %1（空）").arg(group));
    });
    m_hotkeys = new HotkeyManager(this);
    connect(m_hotkeys, &HotkeyManager::triggered, this, &App::onHotkey);
    connect(m_hotkeys, &HotkeyManager::changed, this, &App::updateTrayTexts);
    connect(m_hotkeys, &HotkeyManager::fullscreenPauseChanged, this, &App::updateTrayTexts);
    m_fullscreenPoll = new QTimer(this);
    m_fullscreenPoll->setInterval(200);
    connect(m_fullscreenPoll, &QTimer::timeout, this, [this] {
        m_hotkeys->setPausedForFullscreen(foregroundAppIsFullscreen());
    });

    createTray();
    const QList<Config::Hotkey> taken = m_hotkeys->registerAll();
    if (!taken.isEmpty()) {
        QStringList names;
        for (Config::Hotkey hotkey : taken)
            names << QStringLiteral("%1（%2）").arg(keyText(hotkey), Config::hotkeyName(hotkey));
        notify(tr("快捷键 %1 已被其他程序占用，可在托盘菜单「设置」里更换。").arg(names.join(QStringLiteral("、"))));
    }
    configureFullscreenHotkeyPause(Config::pauseHotkeysInFullscreen());
    return true;
}

void App::handleInstanceMessage(const QString &message)
{
    if (message == QLatin1String("capture")) {
        startCapture();
        return;
    }
    // Launched again while running (e.g. from the Start menu): say where we are.
    const QKeySequence capture = Config::hotkey(Config::Hotkey::Capture);
    notify(capture.isEmpty() ? tr("截影已在运行，可在任务栏通知区域找到它。")
                             : tr("截影已在运行，按 %1 截图；右键托盘图标打开设置。")
                                   .arg(capture.toString(QKeySequence::NativeText)));
}

void App::createTray()
{
    m_tray = new QSystemTrayIcon(makeAppIcon(), this);

    auto *menu = new QMenu();
    m_captureAction = menu->addAction(tr("截图"), this,
                                      [this] { QTimer::singleShot(kMenuCloseDelayMs, this, [this] { startCapture(); }); });
    menu->addAction(tr("自由形状截图"), this, [this] {
        QTimer::singleShot(kMenuCloseDelayMs, this, [this] {
            startCapture();
            if (m_session)
                m_session->setFreeform(true);
        });
    });
    menu->addAction(tr("截取全屏"), this,
                    [this] { QTimer::singleShot(kMenuCloseDelayMs, this, &App::captureScreenUnderCursor); });
    m_repeatCaptureAction = menu->addAction(tr("重截上次选区"), this, [this] {
        QTimer::singleShot(kMenuCloseDelayMs, this, &App::repeatCapture);
    });
    QMenu *delayed = menu->addMenu(tr("延时截图"));
    for (int seconds : {3, 5, 10})
        delayed->addAction(tr("%1 秒后").arg(seconds), this, [this, seconds] { startDelayedCapture(seconds); });

    menu->addSeparator();
    m_pinAction = menu->addAction(tr("贴剪贴板图片"), this, &App::pinClipboard);
    menu->addAction(tr("实时窗口贴图…"), this, &App::showLivePin);
    m_togglePinsAction = menu->addAction(tr("隐藏/显示当前贴图"), this, &App::togglePins);
    QMenu *groups = menu->addMenu(tr("贴图分组"));
    connect(groups, &QMenu::aboutToShow, this, [this, groups] { m_board->fillMenu(groups); });
    m_restoreClicksAction = menu->addAction(tr("恢复贴图的鼠标操作"), this, &App::restorePinClicks);
    QAction *restoreLast = menu->addAction(tr("恢复最近关闭的贴图"), this, &App::restoreLastPin);
    menu->addAction(tr("关闭全部贴图"), this, &App::closeAllPins);

    menu->addSeparator();
    QMenu *recent = menu->addMenu(tr("最近截图"));
    connect(recent, &QMenu::aboutToShow, this, [this, recent] { fillHistoryMenu(recent); });
    menu->addAction(tr("多图拼排…"), this, [this] { showCollage(); });

    connect(menu, &QMenu::aboutToShow, this, [this, restoreLast] {
        m_repeatCaptureAction->setEnabled(m_lastCaptureScreen && !m_lastCaptureArea.bounds.isEmpty());
        restoreLast->setEnabled(m_board->canRestoreLastClosed());
        m_restoreClicksAction->setVisible(m_board->anyClickThrough());
    });
    menu->addSeparator();
    menu->addAction(tr("设置…"), this, &App::showSettings);
    menu->addAction(tr("退出"), qApp, &QApplication::quit);
    connect(qApp, &QObject::destroyed, menu, &QObject::deleteLater);
    m_tray->setContextMenu(menu);

    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger)
            QTimer::singleShot(kMenuCloseDelayMs, this, [this] { startCapture(); });
    });
    updateTrayTexts();
    m_tray->show();
}

void App::updateTrayTexts()
{
    m_captureAction->setShortcut(Config::hotkey(Config::Hotkey::Capture));
    m_repeatCaptureAction->setShortcut(Config::hotkey(Config::Hotkey::RepeatCapture));
    m_pinAction->setShortcut(Config::hotkey(Config::Hotkey::PinClipboard));
    m_togglePinsAction->setShortcut(Config::hotkey(Config::Hotkey::TogglePins));
    QString tip = tr("截影");
    if (!Config::hotkey(Config::Hotkey::Capture).isEmpty())
        tip += QStringLiteral("\n%1 %2").arg(keyText(Config::Hotkey::Capture), tr("截图"));
    if (!Config::hotkey(Config::Hotkey::PinClipboard).isEmpty())
        tip += QStringLiteral("\n%1 %2").arg(keyText(Config::Hotkey::PinClipboard), tr("贴剪贴板图片"));
    if (m_hotkeys->pausedForFullscreen())
        tip += QStringLiteral("\n") + tr("全屏应用期间已暂停全局快捷键");
    m_tray->setToolTip(tip);
}

void App::configureFullscreenHotkeyPause(bool enabled)
{
    if (enabled)
        m_fullscreenPoll->start();
    else
        m_fullscreenPoll->stop();
    m_hotkeys->setPausedForFullscreen(enabled && foregroundAppIsFullscreen());
}

void App::onHotkey(Config::Hotkey hotkey)
{
    // Check again in case the foreground changed since the timer last ran.
    if (m_fullscreenPoll->isActive() && foregroundAppIsFullscreen()) {
        m_hotkeys->setPausedForFullscreen(true);
        return;
    }
    switch (hotkey) {
    case Config::Hotkey::Capture:
        startCapture();
        break;
    case Config::Hotkey::PinClipboard:
        pinClipboard();
        break;
    case Config::Hotkey::LongShot:
        startCapture(CaptureAction::LongShot);
        break;
    case Config::Hotkey::RecordGif:
        startCapture(CaptureAction::RecordGif);
        break;
    case Config::Hotkey::RecordMp4:
        startCapture(CaptureAction::RecordMp4);
        break;
    case Config::Hotkey::TogglePins:
        togglePins();
        break;
    case Config::Hotkey::CaptureScreen:
        captureScreenUnderCursor();
        break;
    case Config::Hotkey::CaptureWindow:
        captureActiveWindow();
        break;
    case Config::Hotkey::RepeatCapture:
        repeatCapture();
        break;
    case Config::Hotkey::NextPinGroup:
        m_board->nextGroup();
        break;
    }
}

void App::captureScreenUnderCursor()
{
    QScreen *screen = QApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QApplication::primaryScreen();
    startCapture(std::nullopt, QRect(screen->geometry().topLeft(), screen->geometry().size() * screen->devicePixelRatio()));
}

void App::captureActiveWindow()
{
    // Read before any overlay takes the foreground.
    const QRect window = foregroundWindowRect();
    if (window.isEmpty())
        captureScreenUnderCursor();
    else
        startCapture(std::nullopt, window);
}

void App::repeatCapture()
{
    if (m_session || m_scrollCapture || m_pinEditor || m_recorder)
        return;

    QScreen *screen = m_lastCaptureScreen.data();
    if (!screen || m_lastCaptureArea.bounds.isEmpty()) {
        notify(tr("请先在当前显示器上截一次图，再重截选区。"));
        return;
    }

    const QPixmap desktop = screen->grabWindow(0);
    QPixmap image = QPixmap::fromImage(m_lastCaptureArea.render(desktop.toImage()));
    if (image.isNull()) {
        notify(tr("无法重截该选区，请重新选择截图区域。"));
        return;
    }
    const qreal dpr = screen->devicePixelRatio();
    image.setDevicePixelRatio(dpr);
    const QPoint topLeft = screen->geometry().topLeft() + (QPointF(m_lastCaptureArea.bounds.topLeft()) / dpr).toPoint();
    deliver(SelectionOverlay::configuredConfirmAction(), image, topLeft);
}

void App::startCapture(std::optional<CaptureAction> autoAction, const QRect &preselect)
{
    if (m_recorder) { // capture and record hotkeys double as "stop recording"
        m_recorder->stop();
        return;
    }
    if (m_session || m_scrollCapture || m_pinEditor)
        return;

    // OCR is likely soon after the first capture; load its models while the user selects.
    static bool ocrWarmedUp = false;
    if (!ocrWarmedUp) {
        ocrWarmedUp = true;
        Ocr::warmUpInBackground();
    }

    m_session = new CaptureSession(autoAction, this);
    m_session->setPreselection(preselect);
    connect(m_session, &CaptureSession::imageAction, this,
            [this](CaptureAction action, const QPixmap &image, const QPoint &topLeft, QScreen *screen,
                   const ScreenshotArea &area) {
                m_lastCaptureScreen = screen;
                m_lastCaptureArea = area;
                deliver(action, image, topLeft);
            });
    connect(m_session, &CaptureSession::regionRequested, this, &App::startRegionMode);
    connect(m_session, &CaptureSession::finished, m_session, &QObject::deleteLater);
    m_session->start();
}

void App::pinClipboard()
{
    QScreen *screen = QApplication::screenAt(QCursor::pos());
    const QList<QImage> images =
        ImageIO::pinnableImages(QApplication::clipboard()->mimeData(), screen ? screen->devicePixelRatio() : 1.0);
    if (images.isEmpty()) {
        notify(tr("剪贴板中没有图片、图片文件或文字。"));
        return;
    }
    if (images.size() == 1) {
        pinAtCursor(images.first());
        return;
    }
    pinImagesAt(images, QCursor::pos());
}

void App::pinImagesAt(const QList<QImage> &images, const QPoint &pos)
{
    QScreen *screen = QApplication::screenAt(pos);
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    QPoint at = pos;
    for (const QImage &image : images) {
        QPixmap pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(dpr);
        addPin(pixmap, at);
        at += QPoint(24, 24); // a fanned-out stack
    }
}

void App::restorePinClicks()
{
    m_board->restoreClicks();
}

void App::startDelayedCapture(int seconds)
{
    notify(tr("%1 秒后截图，可以先打开要截的菜单或提示。").arg(seconds));
    QTimer::singleShot(seconds * 1000, this, [this] { startCapture(); });
}

void App::fillHistoryMenu(QMenu *menu)
{
    menu->clear();
    menu->addAction(tr("打开截图历史…"), this, &App::showHistory);
    menu->addSeparator();
    const QList<QFileInfo> entries = m_history->entries();
    if (entries.isEmpty()) {
        menu->addAction(Config::historySize() > 0 ? tr("（还没有截图）") : tr("（已在设置中关闭）"))->setEnabled(false);
    } else {
        for (const QFileInfo &entry : entries.mid(0, 10)) {
            const QDateTime when = QDateTime::fromString(entry.completeBaseName().left(19), "yyyyMMdd_HHmmss_zzz");
            const QString label = when.isValid() ? when.toString(when.date() == QDate::currentDate() ? "HH:mm:ss"
                                                                                                    : "MM-dd HH:mm")
                                                 : entry.completeBaseName();
            QAction *action = menu->addAction(m_history->thumbnail(entry), tr("贴出 %1").arg(label));
            const QString path = entry.absoluteFilePath();
            connect(action, &QAction::triggered, this, [this, path] { pinAtCursor(QImage(path)); });
        }
    }
    menu->addSeparator();
    menu->addAction(tr("打开历史文件夹"), this, [this] {
        QDir().mkpath(m_history->folder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_history->folder()));
    });
    menu->addAction(tr("清空历史"), m_history, &CaptureHistory::clear)->setEnabled(!entries.isEmpty());
}

void App::showHistory()
{
    if (!m_historyDialog) {
        m_historyDialog = new HistoryDialog(m_history);
        m_historyDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_historyDialog, &HistoryDialog::pinRequested, this, [this](const QPixmap &image) {
            pinAtCursor(image.toImage());
        });
        connect(m_historyDialog, &HistoryDialog::collageRequested, this, &App::showCollage);
    }
    m_historyDialog->show();
    m_historyDialog->raise();
    m_historyDialog->activateWindow();
}

void App::showCollage(const QStringList &paths)
{
    if (!m_collageDialog)
        m_collageDialog = new CollageDialog;
    if (!paths.isEmpty())
        m_collageDialog->addImages(paths);
    m_collageDialog->setWindowState(m_collageDialog->windowState() & ~Qt::WindowMinimized);
    m_collageDialog->show();
    m_collageDialog->raise();
    m_collageDialog->activateWindow();
}

void App::showLivePin()
{
    if (!m_livePinDialog) {
        m_livePinDialog = new LivePinDialog;
        m_livePinDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_livePinDialog, &LivePinDialog::freezeRequested, this, &App::addPin);
    }
    m_livePinDialog->setWindowState(m_livePinDialog->windowState() & ~Qt::WindowMinimized);
    m_livePinDialog->show();
    m_livePinDialog->raise();
    m_livePinDialog->activateWindow();
}

void App::pinAtCursor(const QImage &image)
{
    if (image.isNull())
        return;
    // Show the pixels 1:1 on the screen under the cursor, centered on the cursor.
    const QPoint cursor = QCursor::pos();
    QScreen *screen = QApplication::screenAt(cursor);
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(screen ? screen->devicePixelRatio() : 1.0);
    const QSize size = pixmap.deviceIndependentSize().toSize();
    addPin(pixmap, cursor - QPoint(size.width() / 2, size.height() / 2));
}

void App::togglePins()
{
    m_board->toggleCurrent();
}

void App::closeAllPins()
{
    m_board->closeAll();
    if (m_livePinDialog)
        m_livePinDialog->close();
}

void App::showSettings()
{
    if (!m_settings) {
        m_settings = new SettingsDialog(m_hotkeys);
        connect(m_settings, &SettingsDialog::fullscreenHotkeyPauseEnabled, this, &App::configureFullscreenHotkeyPause);
    }
    m_settings->show();
    m_settings->raise();
    m_settings->activateWindow();
}

void App::addPin(const QPixmap &image, const QPoint &contentTopLeft, qreal initialScale)
{
    auto *pin = new PinWindow(image, contentTopLeft, initialScale);
    connectPinActions(pin);
    m_board->add(pin);
    pin->activateWindow();
}

void App::connectPinActions(PinWindow *pin)
{
    connect(pin, &PinWindow::ocrRequested, this, &App::showOcr);
    connect(pin, &PinWindow::annotateRequested, this, [this, pin] { annotatePin(pin); });
    connect(pin, &PinWindow::beautifyRequested, this, &App::showBeautify);
    connect(pin, &PinWindow::imagesDropped, this, &App::pinImagesAt);
}

void App::restoreLastPin()
{
    if (PinWindow *pin = m_board->restoreLastClosed()) {
        connectPinActions(pin);
        pin->activateWindow();
    }
}

void App::annotatePin(PinWindow *pin)
{
    if (m_session || m_scrollCapture || m_pinEditor)
        return;
    const QRect content = pin->prepareForEditing();
    QScreen *screen = QApplication::screenAt(content.center());
    if (!screen)
        screen = QApplication::primaryScreen();
    const QRect screenArea = screen->geometry();
    if (!screenArea.contains(content)) {
        notify(tr("请先把贴图完整地移到一个屏幕内，再标注。"));
        return;
    }

    // Editor background: the screen as it is, with the pin's own pixels painted exactly where
    // it sits, so the annotated result has the image's full resolution.
    const qreal dpr = screen->devicePixelRatio();
    QImage background = screen->grabWindow(0).toImage();
    background.setDevicePixelRatio(1);
    QImage pixels = pin->image().toImage();
    pixels.setDevicePixelRatio(1);
    const QPoint physicalTopLeft = ((QPointF(content.topLeft() - screenArea.topLeft())) * dpr).toPoint();
    {
        QPainter p(&background);
        p.drawImage(physicalTopLeft, pixels);
    }
    QPixmap shot = QPixmap::fromImage(background);
    shot.setDevicePixelRatio(dpr);

    auto *editor = new SelectionOverlay(screen, shot, {});
    m_pinEditor = editor;
    editor->editFixedRegion(QRect(physicalTopLeft, pixels.size()));
    QPointer<PinWindow> target(pin);
    pin->hide();
    connect(editor, &SelectionOverlay::actionTriggered, this,
            [editor, target](CaptureAction action, const QPixmap &image, const QPoint &) {
                if (action == CaptureAction::Pin && target)
                    target->replaceImage(image);
                editor->close();
            });
    connect(editor, &SelectionOverlay::cancelled, editor, &QWidget::close);
    connect(editor, &QObject::destroyed, this, [target] {
        if (target) {
            target->show();
            target->activateWindow();
        }
    });
    editor->show();
}

void App::showOcr(const QPixmap &image)
{
    auto *window = new OcrWindow(image.toImage());
    window->show();
    window->activateWindow();
}

void App::deliver(CaptureAction action, const QPixmap &image, const QPoint &globalTopLeft)
{
    m_history->add(image.toImage());
    if (Config::autoSaveCaptures() && action != CaptureAction::Save && action != CaptureAction::QuickSave)
        ImageIO::saveImage(image, nullptr, true);
    switch (action) {
    case CaptureAction::Copy:
        ImageIO::copyToClipboard(image);
        break;
    case CaptureAction::Save: {
        const QString saved = ImageIO::saveImage(image);
        if (!saved.isEmpty() && !Config::askWhereToSave()) // no dialog, so say where it went
            notify(tr("已保存：%1").arg(QDir::toNativeSeparators(saved)));
        break;
    }
    case CaptureAction::QuickSave: {
        const QString saved = ImageIO::saveImage(image, nullptr, true);
        if (!saved.isEmpty())
            notify(tr("已保存：%1").arg(QDir::toNativeSeparators(saved)));
        break;
    }
    case CaptureAction::Pin:
        addPin(image, globalTopLeft);
        break;
    case CaptureAction::Ocr:
        showOcr(image);
        break;
    case CaptureAction::Beautify:
        showBeautify(image);
        break;
    default:
        break;
    }
}

void App::showBeautify(const QPixmap &image)
{
    auto *dialog = new BeautifyDialog(image);
    connect(dialog, &BeautifyDialog::actionRequested, this, &App::deliver);
    dialog->show();
    dialog->activateWindow();
}

void App::startRegionMode(CaptureAction action, QScreen *screen, const QRect &physicalRegion)
{
    // Let the closing overlays disappear before the first frame is grabbed.
    QTimer::singleShot(kMenuCloseDelayMs, this, [this, action, screen = QPointer<QScreen>(screen), physicalRegion] {
        if (!screen)
            return;
        if (action == CaptureAction::LongShot) {
            auto *capture = new ScrollCapture(screen, physicalRegion, this);
            m_scrollCapture = capture;
            connect(capture, &ScrollCapture::finished, this,
                    [this, capture](CaptureAction result, const QPixmap &image, const QPoint &topLeft) {
                        capture->deleteLater();
                        deliver(result, image, topLeft);
                    });
            connect(capture, &ScrollCapture::cancelled, capture, &QObject::deleteLater);
            capture->start();
            return;
        }

        const auto format = action == CaptureAction::RecordGif ? Recorder::Format::Gif : Recorder::Format::Mp4;
        auto *recorder = new Recorder(format, screen, physicalRegion, this);
        m_recorder = recorder;
        connect(recorder, &Recorder::finished, this, [this, recorder](const QString &path) {
            recorder->deleteLater();
            saveRecording(path, Recorder::suffix(recorder->format()));
        });
        connect(recorder, &Recorder::failed, this, [this, recorder](const QString &error) {
            recorder->deleteLater();
            notify(tr("录屏失败：%1").arg(error));
        });
        connect(recorder, &Recorder::cancelled, recorder, &QObject::deleteLater);
        recorder->start();
    });
}

void App::saveRecording(const QString &path, const QString &suffix)
{
    const QString saved = ImageIO::saveRecording(path, suffix);
    if (saved.isEmpty()) {
        QFile::remove(path);
        return;
    }
    notify(tr("录屏已保存：%1").arg(QDir::toNativeSeparators(saved)));
}

void App::notify(const QString &message)
{
    m_tray->showMessage(tr("截影"), message, QSystemTrayIcon::Information, 4000);
}
