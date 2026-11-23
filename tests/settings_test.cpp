// Headless tests for settings: Config defaults and file naming, saving without a dialog,
// hotkey validation (duplicates, keys taken by another program, suspension), the settings
// window, and the overlay honouring settings. Uses its own registry key (JieyingTests) and
// briefly registers rarely used global hotkeys (Ctrl+Alt+Shift+F9..F12).
// Usage: settings_test [output-dir]
#include "autostart.h"
#include "capturehistory.h"
#include "capturetoolbar.h"
#include "config.h"
#include "globalhotkeys.h"
#include "hotkeymanager.h"
#include "imageio.h"
#include "pinwindow.h"
#include "selectionoverlay.h"
#include "settingsdialog.h"
#include "translator.h"

#include <QApplication>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDirIterator>
#include <QDir>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <QWheelEvent>

#include <cstdio>

#include <windows.h>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

// Scratch registry keys standing in for ...\CurrentVersion\Run and Task Manager's StartupApproved\Run.
const wchar_t kTestRoot[] = L"Software\\JieyingTests\\Autostart";
const QString kTestRunKey = QStringLiteral("Software\\JieyingTests\\Autostart\\Run");
const QString kTestApprovedKey = QStringLiteral("Software\\JieyingTests\\Autostart\\StartupApproved");

QString runValue()
{
    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(kTestRunKey.utf16()), L"Jieying", RRF_RT_REG_SZ,
                     nullptr, buffer, &size)
        != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(buffer);
}

void testAutostart()
{
    check(!Autostart::isEnabled() && runValue().isEmpty(), "autostart: off by default");
    check(Autostart::setEnabled(true) && Autostart::isEnabled() && runValue() == Autostart::command()
              && Autostart::command().endsWith(QLatin1String("settings_test.exe\" --autostart")),
          "autostart: enabling writes the quoted exe path with --autostart");

    // Task Manager "Disable": the StartupApproved entry starts with 0x03.
    const BYTE disabled[12] = {0x03};
    RegSetKeyValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(kTestApprovedKey.utf16()), L"Jieying", REG_BINARY,
                    disabled, sizeof(disabled));
    check(!Autostart::isEnabled(), "autostart: disabled in Task Manager counts as off");
    check(Autostart::setEnabled(true) && Autostart::isEnabled(), "autostart: enabling again clears Task Manager's mark");

    const std::wstring stale = L"\"C:\\Old Place\\jieying.exe\" --autostart";
    RegSetKeyValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(kTestRunKey.utf16()), L"Jieying", REG_SZ,
                    stale.c_str(), DWORD((stale.size() + 1) * sizeof(wchar_t)));
    Autostart::refresh();
    check(runValue() == Autostart::command(), "autostart: refresh repoints a moved installation");

    check(Autostart::setEnabled(false) && !Autostart::isEnabled() && runValue().isEmpty(),
          "autostart: disabling removes the entry");
    Autostart::refresh();
    check(runValue().isEmpty(), "autostart: refresh never enables on its own");

    HKEY root = nullptr; // the scratch keys really are under Software\JieyingTests
    check(RegOpenKeyExW(HKEY_CURRENT_USER, kTestRoot, 0, KEY_READ, &root) == ERROR_SUCCESS,
          "autostart: test keys live under Software\\JieyingTests\\Autostart");
    if (root)
        RegCloseKey(root);
}

const QKeySequence kKeyA(QStringLiteral("Ctrl+Alt+Shift+F12"));
const QKeySequence kKeyB(QStringLiteral("Ctrl+Alt+Shift+F11"));
const QKeySequence kKeyC(QStringLiteral("Ctrl+Alt+Shift+F10"));
const QKeySequence kKeyTaken(QStringLiteral("Ctrl+Alt+Shift+F9"));

void testDefaultsAndNaming()
{
    check(Config::hotkey(Config::Hotkey::Capture) == QKeySequence(Qt::Key_F1)
              && Config::hotkey(Config::Hotkey::PinClipboard) == QKeySequence(Qt::Key_F3)
              && Config::hotkey(Config::Hotkey::LongShot).isEmpty(),
          "defaults: F1 capture, F3 pin, long screenshot unassigned");
    check(Config::hotkey(Config::Hotkey::TogglePins) == QKeySequence(QStringLiteral("Shift+F3"))
              && Config::currentHotkeyScheme() == Config::HotkeyScheme::Default,
          "defaults: Shift+F3 hides/shows pins; the default scheme is detected");
    check(!Config::pauseHotkeysInFullscreen(), "defaults: fullscreen hotkey pause is opt-in");
    check(Config::hotkey(Config::Hotkey::RepeatCapture).isEmpty(), "defaults: repeat capture hotkey is opt-in");
    check(Config::askWhereToSave() && Config::imageFormat() == "png" && Config::jpgQuality() == 90
              && Config::ocrEngine() == Config::OcrEngine::Auto && Config::gifFps() == 15 && Config::mp4Fps() == 30,
          "defaults: ask where to save, PNG, JPG 90, OCR auto, GIF 15 / MP4 30 fps");

    const QDateTime when(QDate(2026, 10, 29), QTime(9, 5, 7, 42));
    check(Config::expandFileName(Config::defaultFileNameTemplate(), "png", when) == "Jieying_20261029_090507.png",
          "file name: default template");
    check(Config::expandFileName("截图 {yyyy-MM-dd} {HH.mm.ss.zzz}", "jpg", when) == "截图 2026-10-29 09.05.07.042.jpg",
          "file name: several fields and Chinese text");
    check(Config::expandFileName("a/b:c*{HH:mm}", "png", when) == "a_b_c_09_05.png",
          "file name: characters Windows forbids become _");
    check(Config::expandFileName("   ", "gif", when) == "20261029_090507.gif", "file name: blank template falls back");
}

void testDirectSave()
{
    QTemporaryDir dir;
    Config::setSaveDir(dir.path());
    Config::setAskWhereToSave(false);
    Config::setFileNameTemplate("shot");

    QImage image(320, 200, QImage::Format_RGB32);
    QPainter p(&image);
    QLinearGradient gradient(0, 0, 320, 200);
    gradient.setColorAt(0, Qt::red);
    gradient.setColorAt(1, Qt::blue);
    p.fillRect(image.rect(), gradient);
    p.end();
    const QPixmap pixmap = QPixmap::fromImage(image);

    const QString first = ImageIO::saveImage(pixmap);
    const QString second = ImageIO::saveImage(pixmap);
    check(QFileInfo(first).fileName() == "shot.png" && QFileInfo(second).fileName() == "shot_2.png"
              && QFileInfo(first).absolutePath() == QFileInfo(dir.path()).absoluteFilePath(),
          "save without dialog: into the save folder, numbered instead of overwriting");

    Config::setImageFormat("jpg");
    Config::setJpgQuality(95);
    const qint64 high = QFileInfo(ImageIO::saveImage(pixmap)).size();
    Config::setJpgQuality(20);
    const QString lowPath = ImageIO::saveImage(pixmap);
    const qint64 low = QFileInfo(lowPath).size();
    std::printf("      jpg 95%%: %lld bytes, 20%%: %lld bytes\n", high, low);
    check(lowPath.endsWith(".jpg") && low > 0 && low < high, "save without dialog: JPG format and quality apply");

    // WebP, and a quick save that skips the dialog even when the settings ask.
    Config::setImageFormat("webp");
    Config::setJpgQuality(80);
    Config::setAskWhereToSave(true);
    const QString webp = ImageIO::saveImage(pixmap, nullptr, true);
    const QImage back(webp);
    std::printf("      webp: %lld bytes\n", QFileInfo(webp).size());
    check(webp.endsWith(".webp") && back.size() == QSize(320, 200) && qRed(back.pixel(5, 5)) > 200,
          "quick save: WebP written straight into the save folder");
    Config::setAskWhereToSave(false);
    Config::setImageFormat("png");

    const QString source = dir.filePath("recording.tmp");
    QFile file(source);
    check(file.open(QIODevice::WriteOnly), "recording: temp file created");
    file.write("GIF89a");
    file.close();
    const QString moved = ImageIO::saveRecording(source, "gif");
    check(QFileInfo(moved).fileName() == "shot.gif" && !QFileInfo::exists(source), "recording: moved under the template name");

    Config::setAskWhereToSave(true);
    Config::setSaveDir(QString());
}

void testHotkeys()
{
    Config::setHotkey(Config::Hotkey::Capture, kKeyA);
    Config::setHotkey(Config::Hotkey::PinClipboard, kKeyB);
    for (Config::Hotkey h : Config::kHotkeys) { // only the test keys: never grab common keys for real
        if (h != Config::Hotkey::Capture && h != Config::Hotkey::PinClipboard)
            Config::setHotkey(h, QKeySequence());
    }

    HotkeyManager manager;
    check(manager.registerAll().isEmpty() && manager.isRegistered(Config::Hotkey::Capture)
              && manager.isRegistered(Config::Hotkey::PinClipboard),
          "hotkeys: configured keys register");

    QString error = manager.change(Config::Hotkey::LongShot, kKeyA);
    check(error.contains(QStringLiteral("截图")) && Config::hotkey(Config::Hotkey::LongShot).isEmpty(),
          "hotkeys: a key already used by another action is refused");

    error = manager.change(Config::Hotkey::LongShot, QKeySequence(QStringLiteral("Ctrl+Space")));
    check(!error.isEmpty() && Config::hotkey(Config::Hotkey::LongShot).isEmpty(), "hotkeys: unsupported key is refused");

    error = manager.change(Config::Hotkey::LongShot, kKeyC);
    check(error.isEmpty() && manager.isRegistered(Config::Hotkey::LongShot) && Config::hotkey(Config::Hotkey::LongShot) == kKeyC,
          "hotkeys: a free key is registered and saved");

    {
        GlobalHotkeys otherProgram; // stands in for another application holding the key
        otherProgram.add(999, kKeyTaken);
        error = manager.change(Config::Hotkey::RecordGif, kKeyTaken);
        std::printf("      taken -> \"%s\"\n", error.toUtf8().constData());
        check(error.contains(QStringLiteral("占用")) && Config::hotkey(Config::Hotkey::RecordGif).isEmpty(),
              "hotkeys: a key held elsewhere is reported and not saved");

        error = manager.change(Config::Hotkey::Capture, kKeyTaken);
        check(!error.isEmpty() && manager.isRegistered(Config::Hotkey::Capture) && Config::hotkey(Config::Hotkey::Capture) == kKeyA,
              "hotkeys: failed change keeps the previous key working");
    }

    manager.setSuspended(true);
    check(!manager.isRegistered(Config::Hotkey::Capture) && !manager.isRegistered(Config::Hotkey::LongShot),
          "hotkeys: suspended while a key field has focus");
    error = manager.change(Config::Hotkey::RecordMp4, kKeyTaken); // freed again above
    check(error.isEmpty() && !manager.isRegistered(Config::Hotkey::RecordMp4), "hotkeys: change while suspended saves only");
    manager.setSuspended(false);
    check(manager.isRegistered(Config::Hotkey::Capture) && manager.isRegistered(Config::Hotkey::RecordMp4),
          "hotkeys: resuming registers everything again");

    manager.setPausedForFullscreen(true);
    {
        GlobalHotkeys otherProgram;
        check(!manager.isRegistered(Config::Hotkey::Capture) && otherProgram.add(999, kKeyA),
              "hotkeys: fullscreen releases the system-wide shortcut");
    }
    manager.setSuspended(true);
    manager.setSuspended(false);
    check(!manager.isRegistered(Config::Hotkey::Capture), "hotkeys: ending key editing respects fullscreen pause");
    manager.setSuspended(true);
    manager.setPausedForFullscreen(false);
    check(!manager.isRegistered(Config::Hotkey::Capture), "hotkeys: leaving fullscreen respects key editing");
    manager.setSuspended(false);
    check(manager.isRegistered(Config::Hotkey::Capture) && manager.isRegistered(Config::Hotkey::PinClipboard),
          "hotkeys: leaving both pause states restores shortcuts");

    QSignalSpy changed(&manager, &HotkeyManager::changed);
    error = manager.change(Config::Hotkey::LongShot, QKeySequence());
    check(error.isEmpty() && !manager.isRegistered(Config::Hotkey::LongShot) && Config::hotkey(Config::Hotkey::LongShot).isEmpty()
              && changed.size() == 1,
          "hotkeys: clearing unregisters and notifies");
}

void testPinWheel()
{
    QPixmap image(100, 60);
    image.fill(Qt::red);
    auto *pin = new PinWindow(image, {50, 50});
    pin->show();
    const auto wheel = [pin](Qt::KeyboardModifiers modifiers) {
        const QPoint local = pin->rect().center();
        QWheelEvent event(local, pin->mapToGlobal(local), {}, {0, 120}, Qt::NoButton, modifiers, Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(pin, &event);
    };

    const QSize originalSize = pin->size();
    wheel(Qt::NoModifier);
    check(pin->width() > originalSize.width(), "pin wheel: default wheel zooms");
    QTest::keyClick(pin, Qt::Key_Minus, Qt::ControlModifier);
    check(qAbs(pin->windowOpacity() - 0.9) < 0.01, "pin wheel: original opacity shortcut remains available");
    wheel(Qt::ControlModifier);
    check(qAbs(pin->windowOpacity() - 1.0) < 0.01, "pin wheel: default Ctrl+wheel changes opacity");

    Config::setPinWheelAction(false, Config::PinWheelAction::Opacity);
    Config::setPinWheelAction(true, Config::PinWheelAction::Zoom);
    const QRect before = pin->geometry();
    QTest::keyClick(pin, Qt::Key_Minus, Qt::ControlModifier);
    wheel(Qt::NoModifier);
    check(pin->geometry() == before && qAbs(pin->windowOpacity() - 1.0) < 0.01,
          "pin wheel: switching to opacity changes existing pins without resizing");
    wheel(Qt::ControlModifier);
    check(pin->width() > before.width(), "pin wheel: Ctrl+wheel can be configured to zoom");
    Config::setPinWheelAction(false, Config::PinWheelAction::Disabled);
    QTest::keyClick(pin, Qt::Key_Minus, Qt::ControlModifier);
    const qreal opacity = pin->windowOpacity();
    const QRect disabledGeometry = pin->geometry();
    wheel(Qt::NoModifier);
    check(pin->geometry() == disabledGeometry && pin->windowOpacity() == opacity,
          "pin wheel: disabling an input leaves the pin as it was");
    Config::setPinWheelAction(false, Config::PinWheelAction::Zoom);
    Config::setPinWheelAction(true, Config::PinWheelAction::Opacity);
    pin->close();
}

void testPinQuickControls(const QDir &out)
{
    QPixmap image(220, 140);
    image.fill(QColor(38, 92, 148));
    auto *pin = new PinWindow(image, {160, 150});
    pin->show();
    check(QTest::qWaitForWindowExposed(pin), "pin controls: pin opens");

    QWidget *bar = pin->findChild<QWidget *>(QStringLiteral("pinQuickBar"));
    check(!bar || !bar->isVisible(), "pin controls: stays hidden until requested by pointer proximity");
    const QPoint local(35, 12);
    const QPoint pointer = pin->mapToGlobal(local);
    const auto approachEdge = [pin, local, pointer] {
        QCursor::setPos(pointer);
        QMouseEvent move(QEvent::MouseMove, local, pointer, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(pin, &move);
    };
    approachEdge();
    const bool shown = QTest::qWaitFor([&] {
        bar = pin->findChild<QWidget *>(QStringLiteral("pinQuickBar"));
        return bar && bar->isVisible();
    }, 750);
    check(shown, "pin controls: approaching the upper edge reveals the bar");
    if (!shown) {
        pin->close();
        return;
    }

    auto *copy = bar->findChild<QToolButton *>(QStringLiteral("pinQuickCopy"));
    auto *lock = bar->findChild<QToolButton *>(QStringLiteral("pinQuickLock"));
    auto *opacity = bar->findChild<QToolButton *>(QStringLiteral("pinQuickOpacity"));
    auto *more = bar->findChild<QToolButton *>(QStringLiteral("pinQuickMore"));
    check(copy && lock && opacity && more, "pin controls: copy, lock, opacity and More are available");
    if (!copy || !lock || !opacity || !more) {
        pin->close();
        return;
    }

    const QRect pinGeometry = pin->geometry();
    const QRect barGeometry(bar->mapToGlobal(QPoint()), bar->size());
    check(barGeometry.bottom() < pinGeometry.top() + 10,
          "pin controls: the bar leaves the image unobstructed when there is room");
    QApplication::clipboard()->clear();
    copy->click();
    check(QApplication::clipboard()->image() == image.toImage(), "pin controls: Copy uses the original image");

    lock->click();
    check(lock->isCheckable() && lock->isChecked(), "pin controls: Lock displays its active state");
    QAction *opacityPreset = nullptr;
    if (opacity->menu()) {
        for (QAction *action : opacity->menu()->actions()) {
            if (action->text().contains(QStringLiteral("80%")))
                opacityPreset = action;
        }
    }
    if (opacityPreset)
        opacityPreset->trigger();
    check(opacityPreset && qAbs(pin->windowOpacity() - 0.8) < 0.01 && opacity->text().contains(QStringLiteral("80%")),
          "pin controls: opacity presets update the pin and the displayed percentage");

    bool hasCheckedLock = false;
    QObject::connect(pin, &PinWindow::menuAboutToShow, pin, [&](QMenu *menu) {
        for (QAction *action : menu->actions()) {
            if (action->text() == QStringLiteral("锁定位置"))
                hasCheckedLock = action->isChecked();
        }
        QTimer::singleShot(0, menu, &QWidget::close);
    });
    more->click();
    check(hasCheckedLock, "pin controls: More uses the normal context menu and its current lock state");
    check(pin->geometry() == pinGeometry, "pin controls: the controls do not move or resize the picture");
    const QPixmap barPreview = bar->grab();
    const QPoint surfaceSample(qRound(3 * barPreview.devicePixelRatio()), barPreview.height() / 2);
    check(!barPreview.isNull() && barPreview.toImage().pixelColor(surfaceSample).alpha() > 235,
          "pin controls: an opaque surface keeps the controls readable over the desktop");
    barPreview.save(out.filePath(QStringLiteral("pin_quick_controls.png")));

    QCursor::setPos(QPoint(24, 320));
    QEvent leavePin(QEvent::Leave);
    QEvent leaveBar(QEvent::Leave);
    QApplication::sendEvent(pin, &leavePin);
    QApplication::sendEvent(bar, &leaveBar);
    check(QTest::qWaitFor([bar] { return !bar->isVisible(); }, 750),
          "pin controls: leaving the pin and toolbar hides the controls");
    approachEdge();
    check(QTest::qWaitFor([bar] { return bar->isVisible(); }, 750), "pin controls: approaching again reopens the bar");
    pin->setClickThrough(true);
    check(!bar->isVisible(), "pin controls: hides immediately when click-through is enabled");
    pin->close();
}

void testDialog(const QDir &out)
{
    HotkeyManager manager;
    manager.registerAll();
    SettingsDialog dialog(&manager);
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "dialog: opens");
    const QStringList titles = dialog.pageTitles();
    check(titles.size() == 7 && titles.contains(QStringLiteral("翻译")), "dialog: seven pages, translation included");
    for (int i = 0; i < titles.size(); ++i) {
        dialog.showPage(i);
        dialog.grab().save(out.filePath(QStringLiteral("settings_%1.png").arg(i)));
    }

    // Changes apply immediately, without OK.
    QCheckBox *autostart = nullptr;
    for (QCheckBox *candidate : dialog.findChildren<QCheckBox *>()) {
        if (candidate->text().contains(QStringLiteral("开机")))
            autostart = candidate;
    }
    if (autostart)
        autostart->click();
    check(autostart && Autostart::isEnabled(), "dialog: ticking start-with-Windows enables it at once");
    if (autostart)
        autostart->click();
    check(autostart && !Autostart::isEnabled(), "dialog: unticking disables it again");

    auto *wheel = dialog.findChild<QComboBox *>(QStringLiteral("pinWheelAction"));
    auto *ctrlWheel = dialog.findChild<QComboBox *>(QStringLiteral("pinCtrlWheelAction"));
    if (wheel && ctrlWheel) {
        wheel->setCurrentIndex(int(Config::PinWheelAction::Opacity));
        ctrlWheel->setCurrentIndex(int(Config::PinWheelAction::Disabled));
    }
    check(wheel && ctrlWheel && Config::pinWheelAction(false) == Config::PinWheelAction::Opacity
              && Config::pinWheelAction(true) == Config::PinWheelAction::Disabled,
          "dialog: normal and Ctrl+wheel settings save independently");
    Config::setPinWheelAction(false, Config::PinWheelAction::Zoom);
    Config::setPinWheelAction(true, Config::PinWheelAction::Opacity);

    auto *fullscreen = dialog.findChild<QCheckBox *>(QStringLiteral("pauseHotkeysInFullscreen"));
    QSignalSpy pauseChanged(&dialog, &SettingsDialog::fullscreenHotkeyPauseEnabled);
    if (fullscreen)
        fullscreen->click();
    check(fullscreen && Config::pauseHotkeysInFullscreen() && pauseChanged.size() == 1,
          "dialog: fullscreen pause saves immediately and notifies the controller");
    Config::setPauseHotkeysInFullscreen(false);

    Config::setShowMagnifier(true);
    QCheckBox *box = nullptr;
    for (QCheckBox *candidate : dialog.findChildren<QCheckBox *>()) {
        if (candidate->text().contains(QStringLiteral("放大镜")))
            box = candidate;
    }
    if (box)
        box->click();
    check(box && !Config::showMagnifier(), "dialog: unticking the magnifier saves at once");

    QComboBox *engine = nullptr;
    for (QComboBox *combo : dialog.findChildren<QComboBox *>()) {
        if (combo->count() == 3 && combo->itemText(1) == QLatin1String("PaddleOCR"))
            engine = combo;
    }
    if (engine)
        engine->setCurrentIndex(2);
    check(engine && Config::ocrEngine() == Config::OcrEngine::Windows, "dialog: choosing the OCR engine saves at once");
    Config::setOcrEngine(Config::OcrEngine::Auto);
    Config::setShowMagnifier(true);
}

void testOverlay()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QPixmap shot(screen->geometry().size() * screen->devicePixelRatio());
    shot.fill(Qt::darkCyan);

    {
        SelectionOverlay overlay(screen, shot, {});
        overlay.setAutoAction(CaptureAction::RecordGif);
        QSignalSpy region(&overlay, &SelectionOverlay::regionActionTriggered);
        overlay.show();
        check(QTest::qWaitForWindowExposed(&overlay), "overlay: opens");
        QTest::mousePress(&overlay, Qt::LeftButton, {}, {50, 50});
        for (int i = 1; i <= 6; ++i)
            QTest::mouseMove(&overlay, QPoint(50 + 30 * i, 50 + 20 * i));
        QTest::mouseRelease(&overlay, Qt::LeftButton, {}, {230, 170});
        check(region.size() == 1 && region.first().at(0).value<CaptureAction>() == CaptureAction::RecordGif
                  && region.first().at(2).toRect().size() == QSize(181, 121) * screen->devicePixelRatio(),
              "overlay: with an auto action, releasing the selection starts it");
    }
    {
        Config::setConfirmAction(Config::ConfirmAction::Pin);
        SelectionOverlay overlay(screen, shot, {});
        QSignalSpy actions(&overlay, &SelectionOverlay::actionTriggered);
        overlay.show();
        check(QTest::qWaitForWindowExposed(&overlay), "overlay: opens");
        QTest::keyClick(&overlay, Qt::Key_Return); // hovering: Enter takes the whole screen
        check(actions.size() == 1 && actions.first().at(0).value<CaptureAction>() == CaptureAction::Pin,
              "overlay: Enter runs the configured confirm action");
        Config::setConfirmAction(Config::ConfirmAction::Copy);
    }
}

void testRepeatCaptureArea()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QPixmap shot(screen->geometry().size() * screen->devicePixelRatio());
    shot.fill(Qt::red);
    SelectionOverlay overlay(screen, shot, {});
    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "repeat capture: overlay opens");

    const QRect region(33, 45, 16, 12);
    overlay.preselect(region);
    const ScreenshotArea area = overlay.captureArea();
    check(area.bounds == region && area.mask.isEmpty(), "repeat capture: records exact screen-local physical bounds");

    QImage fresh(90, 80, QImage::Format_RGB32);
    fresh.fill(Qt::black);
    QPainter(&fresh).fillRect(region, Qt::green);
    const QImage repeated = area.render(fresh);
    check(repeated.size() == region.size() && repeated.pixelColor(0, 0) == Qt::green
              && repeated.pixelColor(repeated.width() - 1, repeated.height() - 1) == Qt::green,
          "repeat capture: matches the saved area on fresh screen pixels");
    check(area.render(fresh.copy(QRect(0, 0, 40, 40))).isNull(),
          "repeat capture: refuses a region that no longer fits the screen");

    overlay.hide();
    SelectionOverlay freeform(screen, shot, {});
    freeform.show();
    freeform.setFreeform(true);
    QTest::mousePress(&freeform, Qt::LeftButton, {}, {20, 20});
    QTest::mouseMove(&freeform, {50, 20});
    QTest::mouseMove(&freeform, {35, 50});
    QTest::mouseRelease(&freeform, Qt::LeftButton, {}, {20, 20});
    const ScreenshotArea shaped = freeform.captureArea();
    fresh.fill(Qt::blue);
    const QImage masked = shaped.render(fresh);
    check(!shaped.mask.isEmpty() && !masked.isNull() && masked.pixelColor(masked.rect().center()) == Qt::blue
              && masked.pixelColor(0, masked.height() - 1).alpha() == 0,
          "repeat capture: preserves the freeform shape over fresh pixels");
}

void testHistory()
{
    QTemporaryDir dir;
    CaptureHistory history(dir.filePath("history"));
    QSignalSpy changed(&history, &CaptureHistory::changed);
    auto addAndWait = [&](QColor color) {
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(color);
        const qsizetype before = changed.size();
        history.add(image);
        if (!QTest::qWaitFor([&] { return changed.size() > before; }, 5000))
            check(false, "history: capture written in time");
    };

    Config::setHistorySize(3);
    for (QColor color : {Qt::red, Qt::green, Qt::blue, Qt::yellow, Qt::magenta})
        addAndWait(color);
    const QList<QFileInfo> kept = history.entries();
    check(kept.size() == 3, "history: keeps only the configured number of captures");
    check(kept.size() == 3 && QImage(kept.first().absoluteFilePath()).pixelColor(0, 0) == QColor(Qt::magenta)
              && QImage(kept.last().absoluteFilePath()).pixelColor(0, 0) == QColor(Qt::blue),
          "history: newest first, oldest dropped");
    check(!history.thumbnail(kept.first()).isNull(), "history: thumbnails load");

    Config::setHistorySize(0);
    history.add(QImage(10, 10, QImage::Format_RGB32));
    QTest::qWait(300);
    check(history.entries().size() == 3, "history: size 0 stores nothing new");
    history.prune();
    check(history.entries().isEmpty(), "history: shrinking the size prunes old captures");

    Config::setHistorySize(20);
    addAndWait(Qt::cyan);
    history.clear();
    check(history.entries().isEmpty(), "history: clear empties it");
}

// Hotkey schemes. The manager stays suspended, so no real global key (F1, Ctrl+1, ...) is
// registered while the test runs; only the stored keys and scheme detection are checked.
void testSchemes()
{
    HotkeyManager manager;
    manager.setSuspended(true);
    using S = Config::HotkeyScheme;
    using H = Config::Hotkey;

    manager.applyScheme(S::Alternate);
    check(Config::hotkey(H::Capture) == QKeySequence(QStringLiteral("Ctrl+1"))
              && Config::hotkey(H::PinClipboard) == QKeySequence(QStringLiteral("Ctrl+2"))
              && Config::hotkey(H::TogglePins).isEmpty() && Config::currentHotkeyScheme() == S::Alternate,
          "schemes: scheme two binds Ctrl+1 and Ctrl+2 and clears the toggle key");

    manager.applyScheme(S::Default);
    check(Config::hotkey(H::Capture) == QKeySequence(Qt::Key_F1) && Config::hotkey(H::PinClipboard) == QKeySequence(Qt::Key_F3)
              && Config::hotkey(H::TogglePins) == QKeySequence(QStringLiteral("Shift+F3"))
              && Config::currentHotkeyScheme() == S::Default,
          "schemes: scheme one restores F1, F3 and Shift+F3");

    manager.applyScheme(S::Alternate);
    check(manager.change(H::LongShot, QKeySequence(QStringLiteral("Ctrl+3"))).isEmpty()
              && Config::currentHotkeyScheme() == S::Custom,
          "schemes: changing one key makes it custom");
    manager.applyScheme(S::Alternate);
    check(Config::hotkey(H::LongShot).isEmpty() && Config::currentHotkeyScheme() == S::Alternate,
          "schemes: re-applying a scheme resets every key, including extra ones");

    SettingsDialog dialog(&manager);
    dialog.applyHotkeyScheme(S::Default);
    check(Config::hotkey(H::Capture) == QKeySequence(Qt::Key_F1)
              && dialog.schemeShown() == QStringLiteral("方案一（F1 / F3）"),
          "schemes: settings show scheme one after restoring the default keys");
    dialog.applyHotkeyScheme(S::Alternate);
    check(dialog.schemeShown() == QStringLiteral("方案二（Ctrl+1 / Ctrl+2）")
              && Config::hotkey(H::Capture) == QKeySequence(QStringLiteral("Ctrl+1")),
          "schemes: settings show scheme two after applying its keys");
    manager.applyScheme(S::Default);
}

void testIcons(const QDir &out)
{
    // Every embedded SVG must render through Qt's SVG icon engine (a missing plugin gives nulls).
    int icons = 0, broken = 0;
    for (QDirIterator it(QStringLiteral(":/icons")); it.hasNext();) {
        const QImage image = QIcon(it.next()).pixmap(QSize(24, 24)).toImage();
        bool ink = false;
        for (int y = 0; y < image.height() && !ink; ++y)
            for (int x = 0; x < image.width() && !ink; ++x)
                ink = qAlpha(image.pixel(x, y)) > 0;
        ++icons;
        if (!ink) {
            ++broken;
            std::printf("      no pixels: %s\n", qPrintable(it.filePath()));
        }
    }
    std::printf("      %d icons\n", icons);
    check(icons >= 23 && broken == 0, "icons: every toolbar icon renders");

    CaptureToolbar toolbar;
    toolbar.setTool(Tool::Arrow); // shows the colour/size row too
    toolbar.adjustSize();
    toolbar.show();
    check(QTest::qWaitForWindowExposed(&toolbar), "icons: toolbar opens");
    toolbar.grab().save(out.filePath("toolbar_icons.png"));
    std::printf("      toolbar %d px wide\n", toolbar.width());
    check(toolbar.width() < 760, "icons: icon toolbar is compact (under 760 px)");
}

} // namespace

// Translation: request signing (Baidu's documented example, Tencent TC3 checked against an
// independent implementation), response parsing, encrypted keys, and one round trip against a
// local stand-in server. Nothing goes to a real provider.
void testTranslation()
{
    const QUrlQuery form = Translate::baiduForm("2015063000000001", "12345678", "apple", "zh", "1435660288");
    check(form.queryItemValue("sign") == "f89f9594663708c1605f3d736d01d2d4", "translate: Baidu sign matches its documentation");

    const QByteArray body = Translate::tencentBody("Hello", "zh");
    check(body == R"({"ProjectId":0,"Source":"auto","SourceText":"Hello","Target":"zh"})", "translate: Tencent request body");
    const QString auth = Translate::tencentAuthorization("AKIDEXAMPLE", "SECRETKEY", body, 1700000000);
    check(auth == "TC3-HMAC-SHA256 Credential=AKIDEXAMPLE/2023-11-14/tmt/tc3_request, "
                  "SignedHeaders=content-type;host;x-tc-action, "
                  "Signature=a7092d1ea8d54a6272f78a1b6daee8f24c9e23917be9084efb3e7983593c1f55",
          "translate: Tencent TC3 signature");

    QString error;
    check(Translate::parseBaidu(R"({"from":"en","to":"zh","trans_result":[{"src":"a","dst":"甲"},{"src":"b","dst":"乙"}]})",
                                &error) == QStringLiteral("甲\n乙") && error.isEmpty(),
          "translate: Baidu lines joined");
    Translate::parseBaidu(R"({"error_code":"52003","error_msg":"UNAUTHORIZED USER"})", &error);
    check(error.contains("52003") && error.contains(QStringLiteral("密钥")), "translate: Baidu key error explained");
    error.clear();
    check(Translate::parseTencent(R"({"Response":{"TargetText":"你好","Source":"en","Target":"zh","RequestId":"x"}})", &error)
                  == QStringLiteral("你好") && error.isEmpty(),
          "translate: Tencent result read");
    Translate::parseTencent(R"({"Response":{"Error":{"Code":"AuthFailure.SignatureFailure","Message":"bad"},"RequestId":"x"}})",
                            &error);
    check(error.contains("AuthFailure") && error.contains("SecretKey"), "translate: Tencent auth error explained");

    check(Translate::targetFor(QStringLiteral("截图工具 OCR")) == "en" && Translate::targetFor("Hello world") == "zh",
          "translate: Chinese to English, others to Chinese");
    const QStringList pieces = Translate::chunks(QStringLiteral("aaaa\nbbbb\ncccc\n") + QString(25, 'x'), 10);
    check(pieces == QStringList{"aaaa\nbbbb", "cccc", "xxxxxxxxxx", "xxxxxxxxxx", "xxxxx"}, "translate: text cut at line ends");

    Config::setTranslateSecret("baiduKey", "s3cret-key");
    const QString stored = QSettings().value("translate/baiduKey").toString();
    check(Config::translateSecret("baiduKey") == "s3cret-key" && !stored.isEmpty() && !stored.contains("s3cret"),
          "translate: key stored encrypted and read back");

    // A local server answers like Baidu and shows what was sent.
    QTcpServer server;
    server.listen(QHostAddress::LocalHost);
    QByteArray received;
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            received += socket->readAll();
            const int split = received.indexOf("\r\n\r\n");
            if (split < 0)
                return;
            const QByteArray reply = R"({"from":"en","to":"zh","trans_result":[{"src":"Hello","dst":"你好"}]})";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                          + QByteArray::number(reply.size()) + "\r\n\r\n" + reply);
            socket->disconnectFromHost();
        });
    });
    Config::setTranslateProvider(Config::TranslateProvider::Baidu);
    Config::setBaiduAppId("2015063000000001");
    check(Translator::isConfigured(), "translate: Baidu configured with id and key");
    Translator translator;
    translator.setEndpoint(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
    QSignalSpy done(&translator, &Translator::finished);
    translator.translate("Hello");
    done.wait(5000);
    check(done.size() == 1 && done.first().at(0).toString() == QStringLiteral("你好") && done.first().at(1).toString().isEmpty(),
          "translate: round trip with a stand-in server");
    check(received.contains("q=Hello") && received.contains("appid=2015063000000001") && received.contains("sign="),
          "translate: request carries text, app id and signature");
    Config::setTranslateProvider(Config::TranslateProvider::None);
    Config::setTranslateSecret("baiduKey", QString());
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    QCoreApplication::addLibraryPath(QStringLiteral(JIEYING_QT_PLUGIN_DIR));
    QApplication app(argc, argv);
    QApplication::setOrganizationName("JieyingTests"); // never touch the real app's settings
    QApplication::setApplicationName("settings_test");
    QSettings().clear();
    QTranslator qtTranslator; // as in main(): the merged qt_zh_CN.qm windeployqt puts next to the exe
    const bool translated = qtTranslator.load(QLocale(QLocale::Chinese, QLocale::China), QStringLiteral("qt"),
                                              QStringLiteral("_"),
                                              QCoreApplication::applicationDirPath() + QStringLiteral("/translations"))
                            && QApplication::installTranslator(&qtTranslator);
    check(translated && QCoreApplication::translate("QPlatformTheme", "Cancel") != QLatin1String("Cancel"),
          "Qt's own strings are translated to Chinese");
    const QDir out(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath());

    Autostart::setKeysForTesting(kTestRunKey, kTestApprovedKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kTestRoot);

    testDefaultsAndNaming();
    testAutostart();
    testDirectSave();
    testHotkeys();
    testPinWheel();
    testPinQuickControls(out);
    testDialog(out);
    testOverlay();
    testRepeatCaptureArea();
    testIcons(out);
    testTranslation();
    testHistory();
    testSchemes();

    QSettings().clear();
    RegDeleteTreeW(HKEY_CURRENT_USER, kTestRoot);
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
