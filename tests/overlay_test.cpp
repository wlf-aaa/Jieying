// Headless UI test: drives SelectionOverlay on Qt's offscreen platform with synthetic
// events and saves what it renders. Usage: overlay_test [output-dir]
// Mixed-scale screens (from repository root): pass -platform offscreen:configfile=tests/toolbar_screens.json.
#include "beautify.h"
#include "beautifydialog.h"
#include "config.h"
#include "imageio.h"
#include "pinboard.h"
#include "pinwindow.h"
#include "selectionoverlay.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

QPixmap makeShot(const QSize &size)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter p(&image);
    QLinearGradient gradient(0, 0, size.width(), size.height());
    gradient.setColorAt(0, QColor(40, 60, 90));
    gradient.setColorAt(1, QColor(220, 230, 240));
    p.fillRect(image.rect(), gradient);
    p.setPen(Qt::black);
    for (int y = 20; y < size.height(); y += 24)
        p.drawText(10, y, QStringLiteral("Jietu headless test line %1  截图标注测试").arg(y));
    p.end();
    return QPixmap::fromImage(image);
}

void drag(QWidget *w, QPoint from, QPoint to)
{
    QTest::mousePress(w, Qt::LeftButton, {}, from);
    for (int i = 1; i <= 8; ++i)
        QTest::mouseMove(w, from + (to - from) * i / 8);
    QTest::mouseRelease(w, Qt::LeftButton, {}, to);
}

void selectTool(QWidget *w, int n)
{
    QTest::keyClick(w, Qt::Key(Qt::Key_0 + n), Qt::ControlModifier);
}

QToolButton *spotlightButton(QWidget &overlay)
{
    for (QToolButton *button : overlay.findChildren<QToolButton *>()) {
        if (button->toolTip().startsWith(QStringLiteral("聚光")))
            return button;
    }
    return nullptr;
}

QToolButton *toolbarButton(const CaptureToolbar &toolbar, const QString &tooltipPart)
{
    for (QToolButton *button : toolbar.findChildren<QToolButton *>()) {
        if (button->toolTip().contains(tooltipPart))
            return button;
    }
    return nullptr;
}

} // namespace

// Selecting, moving, restyling, deleting and re-editing existing marks.
void testEditing(const QDir &out)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    SelectionOverlay overlay(screen, makeShot(screen->geometry().size() * dpr), {});
    QImage copied;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction, const QPixmap &image, const QPoint &) { copied = image.toImage(); });
    const QPoint origin(40, 40);
    auto pixel = [&](QPoint screenPos) { // color in the copied image at a screen position
        QTest::keyClick(&overlay, Qt::Key_Return);
        return copied.pixelColor((screenPos - origin) * dpr);
    };
    const QColor red("#FF3B30");
    const QColor blue("#007AFF");

    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "editing: overlay exposed");
    drag(&overlay, origin, {640, 440});
    selectTool(&overlay, 1);
    drag(&overlay, {100, 100}, {200, 180}); // red rectangle, 4 px border on x = 100

    const QPoint oldEdge(100, 140), newEdge(150, 190);
    check(pixel(oldEdge) == red, "editing: rectangle drawn");

    // With the rectangle tool still active, dragging the border moves the mark instead of drawing.
    drag(&overlay, oldEdge, newEdge);
    check(pixel(oldEdge) != red && pixel(newEdge) == red, "editing: dragging a mark's border moves it");
    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);
    check(pixel(oldEdge) == red && pixel(newEdge) != red, "editing: one Ctrl+Z undoes the whole drag");
    QTest::keyClick(&overlay, Qt::Key_Y, Qt::ControlModifier);
    check(pixel(newEdge) == red, "editing: Ctrl+Y redoes it");

    // Selected mark + a colour swatch: the mark is recoloured.
    QTest::mouseClick(&overlay, Qt::LeftButton, {}, newEdge);
    QToolButton *blueSwatch = nullptr;
    for (QToolButton *button : overlay.findChildren<QToolButton *>()) {
        if (button->toolTip() == blue.name().toUpper())
            blueSwatch = button;
    }
    if (blueSwatch)
        blueSwatch->click();
    overlay.grab().save(out.filePath("m2_4_selected_mark.png"));
    check(blueSwatch && pixel(newEdge) == blue, "editing: a swatch recolours the selected mark");
    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);
    check(pixel(newEdge) == red, "editing: recolouring is undoable");

    // Inside the rectangle (away from its border) the active tool still draws a new mark, in the
    // colour picked last (blue; undo reverted the mark, not the toolbar).
    drag(&overlay, {170, 200}, {190, 245});
    check(pixel({170, 240}) == blue, "editing: drawing inside an existing rectangle still works");

    // Delete removes the selected mark.
    QTest::mouseClick(&overlay, Qt::LeftButton, {}, newEdge);
    QTest::keyClick(&overlay, Qt::Key_Delete);
    check(pixel(newEdge) != red, "editing: Delete removes the selected mark");

    // Double-clicking a text mark reopens it in the editor.
    selectTool(&overlay, 5);
    QTest::mouseClick(&overlay, Qt::LeftButton, {}, {300, 300});
    if (auto *edit = overlay.findChild<QPlainTextEdit *>()) {
        QTest::keyClicks(edit, "abc");
        QTest::keyClick(edit, Qt::Key_Escape);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::mouseDClick(&overlay, Qt::LeftButton, {}, {305, 305});
    auto *reopened = overlay.findChild<QPlainTextEdit *>();
    check(reopened && reopened->toPlainText() == "abc", "editing: double-click reopens a text mark with its text");
}

// Line, highlighter, blur, fill and eraser on a white page with one black bar.
void testMoreTools(const QDir &out)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    QImage page(screen->geometry().size() * dpr, QImage::Format_RGB32);
    page.fill(Qt::white);
    QPainter(&page).fillRect(QRect(0, qRound(200 * dpr), page.width(), qRound(10 * dpr)), Qt::black);
    SelectionOverlay overlay(screen, QPixmap::fromImage(page), {});
    QImage copied;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction, const QPixmap &image, const QPoint &) { copied = image.toImage(); });
    const QPoint origin(40, 40);
    auto copy = [&] {
        QTest::keyClick(&overlay, Qt::Key_Return);
        return copied;
    };
    auto pixel = [&](QPoint screenPos) { return copy().pixelColor((screenPos - origin) * dpr); };
    auto button = [&](const QString &tooltipStart) -> QToolButton * {
        for (QToolButton *b : overlay.findChildren<QToolButton *>()) {
            if (b->toolTip().startsWith(tooltipStart))
                return b;
        }
        return nullptr;
    };
    // QTest's mouseMove carries no modifiers; Shift has to ride on real events.
    auto shiftDrag = [&](QPoint from, QPoint to) {
        auto send = [&](QEvent::Type type, QPoint pos, Qt::MouseButtons buttons) {
            QMouseEvent event(type, pos, overlay.mapToGlobal(pos), Qt::LeftButton, buttons, Qt::ShiftModifier);
            QApplication::sendEvent(&overlay, &event);
        };
        send(QEvent::MouseButtonPress, from, Qt::LeftButton);
        for (int i = 1; i <= 8; ++i)
            send(QEvent::MouseMove, from + (to - from) * i / 8, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::NoButton);
    };
    const QColor red("#FF3B30");

    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "tools: overlay exposed");
    drag(&overlay, origin, {640, 440});

    selectTool(&overlay, 8);
    drag(&overlay, {60, 60}, {300, 60});
    check(pixel({180, 60}) == red && pixel({180, 70}) == Qt::white, "tools: Ctrl+8 draws a straight line");
    shiftDrag({60, 100}, {300, 108});
    check(pixel({290, 100}) == red && pixel({290, 108}) != red, "tools: Shift snaps a line to horizontal");

    selectTool(&overlay, 9);
    if (QToolButton *yellow = button(QStringLiteral("#FFCC00")))
        yellow->click();
    drag(&overlay, {60, 205}, {300, 205});
    const QColor onBar = pixel({180, 205});
    const QColor besideBar = pixel({180, 214});
    std::printf("      highlighter on black %s, on white %s\n", qPrintable(onBar.name()), qPrintable(besideBar.name()));
    check(onBar.lightness() < 40, "tools: highlighter keeps dark text dark (multiply)");
    check(besideBar.red() > 220 && besideBar.blue() < 160, "tools: highlighter tints white paper yellow");

    selectTool(&overlay, 0);
    drag(&overlay, {340, 170}, {600, 240});
    const QColor barEdge = pixel({470, 197});
    std::printf("      blur just above the bar: %s\n", qPrintable(barEdge.name()));
    check(barEdge.lightness() > 30 && barEdge.lightness() < 225, "tools: Ctrl+0 blurs the bar's sharp edge");

    if (QToolButton *redSwatch = button(red.name().toUpper()))
        redSwatch->click();
    QToolButton *fill = button(QStringLiteral("填充"));
    check(fill != nullptr, "tools: fill toggle in the style row");
    if (fill)
        fill->click();
    selectTool(&overlay, 1);
    drag(&overlay, {60, 300}, {140, 360});
    check(pixel({100, 330}) == red, "tools: fill makes a solid rectangle");
    selectTool(&overlay, 5);
    QTest::mouseClick(&overlay, Qt::LeftButton, {}, {200, 320});
    if (auto *edit = overlay.findChild<QPlainTextEdit *>()) {
        QTest::keyClicks(edit, "Hi");
        QTest::keyClick(edit, Qt::Key_Escape);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(pixel({197, 324}) == red, "tools: fill puts text on a colored box");
    if (fill)
        fill->click();
    overlay.grab().save(out.filePath("m2_5_more_tools.png"));

    QToolButton *eraser = button(QStringLiteral("橡皮擦"));
    check(eraser != nullptr, "tools: eraser button");
    if (eraser)
        eraser->click();
    drag(&overlay, {180, 50}, {180, 220}); // across the line, the snapped line and the highlighter
    check(pixel({180, 60}) == Qt::white && pixel({290, 100}) == Qt::white && pixel({180, 214}) == Qt::white,
          "tools: one eraser stroke removes every mark it crosses");
    check(pixel({100, 330}) == red, "tools: the eraser leaves marks it did not touch");
    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);
    check(pixel({180, 60}) == red && pixel({290, 100}) == red, "tools: one Ctrl+Z restores the whole eraser stroke");
}

void testSpotlight()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    QPixmap shot(screen->geometry().size() * dpr);
    shot.fill(Qt::white);
    SelectionOverlay overlay(screen, shot, {});
    QImage copied;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction, const QPixmap &image, const QPoint &) { copied = image.toImage(); });
    const QPoint origin(40, 40);
    auto copy = [&] { QTest::keyClick(&overlay, Qt::Key_Return); };
    auto pixel = [&](const QPoint &pos) { return copied.pixelColor((pos - origin) * dpr); };
    auto dimmed = [](const QColor &color) { return color.lightness() > 0 && color.lightness() < 220; };

    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "spotlight: overlay exposed");
    drag(&overlay, origin, {540, 380});

    QToolButton *spotlight = spotlightButton(overlay);
    check(spotlight && !spotlight->icon().isNull(), "spotlight: toolbar button has its icon");
    if (!spotlight)
        return;

    selectTool(&overlay, 1);
    drag(&overlay, {330, 100}, {420, 200});
    copy();
    const QColor ordinaryMark = pixel({330, 150});
    const QPoint outside(485, 320), first(105, 110), overlap(185, 155), second(245, 195);
    QTest::mouseClick(spotlight, Qt::LeftButton);
    QTest::mouseClick(&overlay, Qt::LeftButton, {}, first);
    copy();
    check(pixel(outside) == Qt::white, "spotlight: a click leaves the image unchanged");

    QTest::mousePress(&overlay, Qt::LeftButton, {}, {85, 90});
    QTest::mouseMove(&overlay, {210, 180});
    const QImage draft = overlay.grab().toImage();
    check(draft.pixelColor(first * dpr) == Qt::white && dimmed(draft.pixelColor(outside * dpr)),
          "spotlight: drawing previews the clear area and its surroundings");
    QTest::mouseRelease(&overlay, Qt::LeftButton, {}, {210, 180});
    drag(&overlay, {275, 230}, {160, 130});
    copy();
    check(pixel(first) == Qt::white && pixel(overlap) == Qt::white && pixel(second) == Qt::white
              && dimmed(pixel(outside)),
          "spotlight: two overlapping frames share one mask and preserve both interiors");
    check(pixel(outside) == draft.pixelColor(outside * dpr) && pixel({330, 150}) == ordinaryMark
              && ordinaryMark != Qt::white,
          "spotlight: preview and export use the same dimming and ordinary marks stay above it");

    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);
    copy();
    check(pixel(first) == Qt::white && dimmed(pixel(second)), "spotlight: undo removes the last frame");
    QTest::keyClick(&overlay, Qt::Key_Y, Qt::ControlModifier);
    copy();
    check(pixel(second) == Qt::white, "spotlight: redo restores the last frame");

    drag(&overlay, {85, 130}, {105, 165});
    copy();
    check(dimmed(pixel(first)) && pixel({130, 180}) == Qt::white, "spotlight: dragging the border moves the frame");
    drag(&overlay, {105, 125}, {90, 100});
    copy();
    check(pixel(first) == Qt::white && pixel({130, 180}) == Qt::white, "spotlight: dragging a handle resizes the frame");
    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);
    copy();
    check(dimmed(pixel(first)) && pixel({130, 180}) == Qt::white, "spotlight: undo restores the pre-resize frame");

    QTest::mouseClick(&overlay, Qt::LeftButton, {}, {105, 160});
    QTest::keyClick(&overlay, Qt::Key_Delete);
    copy();
    check(dimmed(pixel({130, 180})) && pixel(second) == Qt::white, "spotlight: Delete removes only the selected frame");
    QTest::mouseClick(spotlight, Qt::LeftButton);
    QTest::keyClick(&overlay, Qt::Key_9, Qt::ShiftModifier);
    drag(&overlay, {285, 175}, {265, 175});
    copy();
    check(pixel(outside) == Qt::white, "spotlight: erasing the last frame removes the mask");
}

// Check keyboard shortcuts for color picking, selection movement, tools and OCR.
void testBorrowedKeys()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    QImage page(screen->geometry().size() * dpr, QImage::Format_RGB32);
    page.fill(QColor(10, 20, 30));
    SelectionOverlay overlay(screen, QPixmap::fromImage(page), {});
    QList<std::pair<CaptureAction, QPoint>> actions;
    QImage copied;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction action, const QPixmap &image, const QPoint &topLeft) {
                         actions.append({action, topLeft});
                         copied = image.toImage();
                     });
    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "keys: overlay exposed");

    QTest::mouseMove(&overlay, {100, 100});
    QTest::keyClick(&overlay, Qt::Key_C);
    check(QGuiApplication::clipboard()->text() == QStringLiteral("#0A141E"), "keys: C copies the color as HEX");
    QTest::keyClick(&overlay, Qt::Key_Shift);
    QTest::keyClick(&overlay, Qt::Key_C);
    check(QGuiApplication::clipboard()->text() == QStringLiteral("10, 20, 30"), "keys: after Shift, C copies R, G, B");

    drag(&overlay, {40, 40}, {240, 240});
    QTest::keyClick(&overlay, Qt::Key_D);
    QTest::keyClick(&overlay, Qt::Key_S, Qt::ShiftModifier);
    QTest::keyClick(&overlay, Qt::Key_Return);
    check(!actions.isEmpty() && actions.last().second == QPoint(41, 50), "keys: D and Shift+S nudge the selection");

    QTest::keyClick(&overlay, Qt::Key_2, Qt::ShiftModifier);
    drag(&overlay, {60, 100}, {200, 100});
    QTest::keyClick(&overlay, Qt::Key_Return);
    const QPoint onLine = (QPoint(130, 100) - QPoint(41, 50)) * dpr;
    check(copied.pixelColor(onLine) == QColor("#FF3B30"), "keys: Shift+2 picks the line tool");

    QTest::keyClick(&overlay, Qt::Key_O);
    check(actions.last().first == CaptureAction::Ocr, "keys: plain O recognizes text");
    QTest::keyClick(&overlay, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
    check(actions.last().first == CaptureAction::QuickSave, "keys: Ctrl+Shift+S saves without asking");
}

void testPinPositionLock()
{
    const QPixmap image = makeShot({160, 100});
    auto *pin = new PinWindow(image, {300, 300});
    pin->show();
    check(QTest::qWaitForWindowExposed(pin), "pin position lock: pin shown");

    const auto withMenu = [pin](auto useMenu) {
        const auto connection = QObject::connect(pin, &PinWindow::menuAboutToShow, pin,
                                                 [useMenu](QMenu *menu) {
            QTimer::singleShot(0, menu, [menu, useMenu] {
                useMenu(menu);
                menu->close();
            });
        });
        const QPoint local = pin->rect().center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, local, pin->mapToGlobal(local));
        QApplication::sendEvent(pin, &event);
        QObject::disconnect(connection);
    };
    const auto actionByText = [](QMenu *menu, const QString &text) -> QAction * {
        for (QAction *action : menu->actions()) {
            if (action->text() == text)
                return action;
        }
        return nullptr;
    };

    withMenu([&](QMenu *menu) {
        QAction *lock = actionByText(menu, QStringLiteral("锁定位置"));
        check(lock && lock->isCheckable() && !lock->isChecked(),
              "pin position lock: menu action starts unchecked");
        if (lock)
            lock->trigger();
    });

    const QPoint start = pin->pos();
    drag(pin, {30, 30}, {65, 50});
    check(pin->pos() == start, "pin position lock: dragging leaves the window in place");

    QApplication::clipboard()->clear();
    withMenu([&](QMenu *menu) {
        QAction *copy = actionByText(menu, QStringLiteral("复制"));
        if (copy)
            copy->trigger();
    });
    check(QApplication::clipboard()->image() == image.toImage(),
          "pin position lock: Copy still puts the pin image on the clipboard");

    const QTemporaryDir saveFolder;
    check(saveFolder.isValid(), "pin position lock: temporary save folder created");
    if (saveFolder.isValid()) {
        const QString previousSaveDir = Config::saveDir();
        const bool previousAsk = Config::askWhereToSave();
        Config::setSaveDir(saveFolder.path());
        Config::setAskWhereToSave(false);
        withMenu([&](QMenu *menu) {
            QAction *save = actionByText(menu, QStringLiteral("另存为…"));
            if (save)
                save->trigger();
        });
        Config::setAskWhereToSave(previousAsk);
        Config::setSaveDir(previousSaveDir);
        const QDir dir(saveFolder.path());
        const QStringList files = dir.entryList(QDir::Files);
        check(files.size() == 1 && QImage(dir.filePath(files.first())) == image.toImage(),
              "pin position lock: Save As still writes the pin image");
    }

    withMenu([&](QMenu *menu) {
        QAction *lock = actionByText(menu, QStringLiteral("锁定位置"));
        check(lock && lock->isChecked(), "pin position lock: menu action shows the locked state");
        if (lock)
            lock->trigger();
    });
    const QPoint unlocked = pin->pos();
    drag(pin, {30, 30}, {65, 50});
    check(pin->pos() != unlocked, "pin position lock: dragging moves the window after unlocking");
    pin->close();
}

// Pins: rotate, flip, click-through, Delete; what F3 pins from files and text.
void testPins(const QDir &out)
{
    QImage source(200, 100, QImage::Format_RGB32);
    source.fill(Qt::white);
    QPainter(&source).fillRect(0, 0, 20, 20, Qt::red); // marks the top-left corner
    QPointer<PinWindow> pin = new PinWindow(QPixmap::fromImage(source), {300, 300});
    pin->show();
    check(QTest::qWaitForWindowExposed(pin), "pins: pin shown");
    const QPoint center = pin->geometry().center();

    QTest::keyClick(pin, Qt::Key_R);
    QImage turned = pin->image().toImage();
    check(turned.size() == QSize(100, 200) && turned.pixelColor(95, 5) == Qt::red,
          "pins: R turns clockwise (top-left corner goes top-right)");
    check((pin->geometry().center() - center).manhattanLength() <= 2, "pins: turning keeps the pin's center");
    QTest::keyClick(pin, Qt::Key_R, Qt::ShiftModifier);
    QTest::keyClick(pin, Qt::Key_H);
    check(pin->image().toImage().pixelColor(195, 5) == Qt::red, "pins: Shift+R turns back, H mirrors left-right");
    QTest::keyClick(pin, Qt::Key_V);
    check(pin->image().toImage().pixelColor(195, 95) == Qt::red, "pins: V mirrors top-bottom");

    QTest::keyClick(pin, Qt::Key_P);
    check(pin->clickThrough() && pin->windowHandle()->flags().testFlag(Qt::WindowTransparentForInput)
              && pin->isVisible(),
          "pins: P makes the pin click-through and keeps it shown");
    pin->setClickThrough(false);
    check(!pin->windowHandle()->flags().testFlag(Qt::WindowTransparentForInput), "pins: click-through can be undone");
    pin->grab().save(out.filePath("pin_turned.png"));

    // Crop: X, drag the part to keep (10 px is the shadow margin), Enter.
    QPointer<PinWindow> crop = new PinWindow(QPixmap::fromImage(source), {500, 300});
    crop->show();
    QTest::qWaitForWindowExposed(crop);
    const QPoint before = crop->mapToGlobal(QPoint(10 + 5, 10 + 5));
    QTest::keyClick(crop, Qt::Key_X);
    QTest::mousePress(crop, Qt::LeftButton, {}, {15, 15});
    QTest::mouseMove(crop, {40, 30});
    QTest::mouseMove(crop, {65, 55});
    QTest::mouseRelease(crop, Qt::LeftButton, {}, {65, 55});
    QTest::keyClick(crop, Qt::Key_Escape);
    check(crop->image().size() == QSize(200, 100), "pins: Esc leaves the crop undone");
    QTest::keyClick(crop, Qt::Key_X);
    QTest::mousePress(crop, Qt::LeftButton, {}, {15, 15});
    QTest::mouseMove(crop, {40, 30});
    QTest::mouseMove(crop, {65, 55});
    QTest::mouseRelease(crop, Qt::LeftButton, {}, {65, 55});
    QTest::keyClick(crop, Qt::Key_Return);
    const QImage kept = crop->image().toImage();
    std::printf("      cropped to %dx%d\n", kept.width(), kept.height());
    check(kept.size() == QSize(50, 40) && kept.pixelColor(2, 2) == Qt::red && kept.pixelColor(40, 30) == Qt::white,
          "pins: X + drag + Enter keeps the dragged part");
    check(crop->mapToGlobal(QPoint(10, 10)) == before, "pins: the kept part stays where it was on screen");
    crop->close();

    pin->activateWindow();
    pin->setFocus();
    QTest::keyClick(pin, Qt::Key_Delete);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(pin.isNull(), "pins: Delete closes the pin");

    // Clipboard / drop contents.
    QMimeData files;
    QImage blue(30, 20, QImage::Format_RGB32);
    blue.fill(Qt::blue);
    blue.save(out.filePath("pin_a.png"));
    blue.scaled(60, 40).save(out.filePath("pin_b.jpg"));
    QFile note(out.filePath("pin_note.txt"));
    if (note.open(QIODevice::WriteOnly))
        note.write("not an image");
    note.close();
    files.setUrls({QUrl::fromLocalFile(out.filePath("pin_a.png")), QUrl::fromLocalFile(out.filePath("pin_note.txt")),
                   QUrl::fromLocalFile(out.filePath("pin_b.jpg"))});
    const QList<QImage> fromFiles = ImageIO::pinnableImages(&files, 1.0);
    check(fromFiles.size() == 2 && fromFiles[1].size() == QSize(60, 40), "pins: copied image files are pinned, others skipped");

    QMimeData hex;
    hex.setText(QStringLiteral("#FF8800"));
    const QList<QImage> swatch = ImageIO::pinnableImages(&hex, 1.0);
    check(swatch.size() == 1 && swatch[0].pixelColor(3, 3) == QColor("#FF8800"), "pins: a color code becomes a swatch");
    QMimeData triple;
    triple.setText(QStringLiteral("rgb(10, 20, 30)"));
    const QList<QImage> rgbSwatch = ImageIO::pinnableImages(&triple, 1.0);
    check(rgbSwatch.size() == 1 && rgbSwatch[0].pixelColor(3, 3) == QColor(10, 20, 30), "pins: rgb(...) becomes a swatch");

    QMimeData text;
    text.setText(QStringLiteral("贴一段文字\nsecond line"));
    const QList<QImage> rendered = ImageIO::pinnableImages(&text, 2.0);
    bool ink = false;
    if (rendered.size() == 1) {
        for (int y = 0; y < rendered[0].height() && !ink; ++y)
            for (int x = 0; x < rendered[0].width() && !ink; ++x)
                ink = rendered[0].pixelColor(x, y).lightness() < 100;
        rendered[0].save(out.filePath("pin_text.png"));
    }
    check(rendered.size() == 1 && ink && rendered[0].pixelColor(1, 1) == Qt::white && rendered[0].height() > 60,
          "pins: text is rendered as an image (at 2x)");
    QMimeData empty;
    check(ImageIO::pinnableImages(&empty, 1.0).isEmpty(), "pins: nothing to pin in an empty clipboard");
}

// Pin groups: only the current group is on screen.
void testPinGroups()
{
    QImage dot(40, 40, QImage::Format_RGB32);
    dot.fill(Qt::darkGreen);
    PinBoard board;
    QPointer<PinWindow> a = new PinWindow(QPixmap::fromImage(dot), {100, 100});
    QPointer<PinWindow> b = new PinWindow(QPixmap::fromImage(dot), {200, 100});
    board.add(a);
    board.add(b);
    check(a->isVisible() && b->isVisible() && board.currentGroup() == 1, "groups: new pins join group 1");

    QSignalSpy shown(&board, &PinBoard::groupShown);
    board.nextGroup();
    check(board.currentGroup() == 2 && !a->isVisible() && !b->isVisible(), "groups: next group after the last is a fresh, empty one");
    QPointer<PinWindow> c = new PinWindow(QPixmap::fromImage(dot), {300, 100});
    board.add(c);
    check(c->isVisible() && c->group() == 2, "groups: a pin made now joins group 2");
    board.nextGroup(); // to empty group 3
    board.nextGroup(); // wraps to 1
    check(board.currentGroup() == 1 && a->isVisible() && b->isVisible() && !c->isVisible(), "groups: switching wraps back to group 1");
    check(shown.size() == 3 && shown.last().at(0).toInt() == 1 && shown.last().at(1).toInt() == 2, "groups: each switch is announced");

    board.toggleCurrent();
    check(!a->isVisible() && !b->isVisible(), "groups: Shift+F3 hides the current group");
    board.toggleCurrent();
    check(a->isVisible() && b->isVisible() && !c->isVisible(), "groups: and shows it again, other groups stay hidden");

    board.moveToGroup(b, 2);
    check(!b->isVisible() && board.count(2) == 2 && board.groups() == QList<int>({1, 2}), "groups: moving a pin out hides it");
    board.showGroup(2);
    check(!a->isVisible() && b->isVisible() && c->isVisible(), "groups: picking a group from the menu shows it");

    board.closeAll();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(a.isNull() && b.isNull() && c.isNull() && board.currentGroup() == 1, "groups: closing all clears every group");
}

void testRestoreClosedPin()
{
    PinBoard board;
    check(!board.canRestoreLastClosed() && !board.restoreLastClosed(),
          "restore pin: starts without a closed image");

    QImage source(200, 100, QImage::Format_RGB32);
    source.fill(Qt::white);
    QPainter(&source).fillRect(0, 0, 20, 20, Qt::red);
    QPixmap image = QPixmap::fromImage(source);
    image.setDevicePixelRatio(2.0);
    board.add(new PinWindow(image, {100, 100}));
    board.toggleCurrent();
    board.showGroup(2);
    check(!board.canRestoreLastClosed(), "restore pin: hiding and switching groups do not make a restore point");

    QPointer<PinWindow> closed = new PinWindow(image, {300, 300});
    board.add(closed);
    closed->rotate(90);
    const QPoint anchor(10, 10);
    QWheelEvent zoom(anchor, closed->mapToGlobal(anchor), {}, {0, 120}, Qt::NoButton,
                     Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(closed, &zoom);
    QTest::keyClick(closed, Qt::Key_Minus, Qt::ControlModifier);
    const qreal opacity = closed->windowOpacity();
    check(opacity < 1.0 && closed->size() == QSize(75, 130), "restore pin: fixture changes opacity and zoom");

    QObject::connect(closed, &PinWindow::menuAboutToShow, closed, [](QMenu *menu) {
        QTimer::singleShot(0, menu, [menu] {
            for (QAction *action : menu->actions()) {
                if (action->text() == QStringLiteral("锁定位置"))
                    action->trigger();
            }
            menu->close();
        });
    });
    const QPoint local = closed->rect().center();
    QContextMenuEvent menu(QContextMenuEvent::Mouse, local, closed->mapToGlobal(local));
    QApplication::sendEvent(closed, &menu);

    const QRect geometry = closed->geometry();
    QTest::keyClick(closed, Qt::Key_Escape);
    check(closed && !closed->isVisible() && board.canRestoreLastClosed() && board.count(2) == 0,
          "restore pin: Escape remembers the pin and removes it from the group immediately");
    board.showGroup(1);
    PinWindow *restored = board.restoreLastClosed();
    check(restored && restored != closed && restored->isVisible() && board.currentGroup() == 2
              && board.count(2) == 1 && restored->group() == 2,
          "restore pin: reopens the image in its original group before the old window is deleted");
    if (restored) {
        const QImage rotated = restored->image().toImage();
        check(rotated.size() == QSize(100, 200) && rotated.pixelColor(95, 5) == Qt::red
                  && rotated.pixelColor(5, 5) == Qt::white && restored->image().devicePixelRatio() == 2.0,
              "restore pin: preserves rotated pixels and image pixel density");
        check(restored->geometry() == geometry && restored->windowOpacity() == opacity,
              "restore pin: preserves location, zoom and opacity");
        const QPoint start = restored->pos();
        drag(restored, {30, 30}, {55, 50});
        check(restored->pos() == start, "restore pin: keeps the pin's location locked");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(closed.isNull() && board.pins().contains(restored) && !board.canRestoreLastClosed()
                  && !board.restoreLastClosed(),
              "restore pin: the restore point is consumed and old window deletion leaves the new pin intact");
        QTest::mouseDClick(restored, Qt::LeftButton, {}, restored->rect().center());
        check(board.canRestoreLastClosed() && board.count(2) == 0,
              "restore pin: double-clicking the restored pin closes it and remembers it again");
    }
    board.closeAll();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void testRestoreAfterClosingAllPins()
{
    const QPixmap image = makeShot({160, 100});
    PinBoard board;
    board.add(new PinWindow(image, {100, 100}));
    board.showGroup(2);
    auto *last = new PinWindow(image, {200, 200});
    board.add(last);
    last->flip(true);
    const QImage expected = last->image().toImage();
    last->setClickThrough(true);
    last->move({100000, 100000});
    const QRect workArea = QGuiApplication::primaryScreen()->availableGeometry();
    check(!workArea.intersects(last->geometry()), "restore pin: fixture is outside the primary screen's work area");

    board.closeAll();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    PinWindow *restored = board.restoreLastClosed();
    check(restored && restored->group() == 2 && restored->image().toImage() == expected
              && !board.canRestoreLastClosed() && board.pins().size() == 1,
          "restore pin: closing all remembers only the final pin closed");
    check(restored && !restored->clickThrough() && !board.anyClickThrough(),
          "restore pin: previously click-through pins reopen with mouse interaction enabled");
    check(restored && workArea.contains(restored->geometry().center()),
          "restore pin: a pin from an unavailable screen moves into the primary screen's work area");
    board.closeAll();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// Ruler marks and freeform (hand-drawn) regions.
void testRulerAndFreeform(const QDir &out)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    QImage page(screen->geometry().size() * dpr, QImage::Format_RGB32);
    page.fill(Qt::white);
    QImage copied;
    auto connectCopy = [&copied](SelectionOverlay &overlay) {
        QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                         [&copied](CaptureAction, const QPixmap &image, const QPoint &) { copied = image.toImage(); });
    };

    {
        SelectionOverlay overlay(screen, QPixmap::fromImage(page), {});
        connectCopy(overlay);
        overlay.show();
        QTest::qWaitForWindowExposed(&overlay);
        drag(&overlay, {40, 40}, {440, 340});
        QToolButton *ruler = nullptr;
        for (QToolButton *b : overlay.findChildren<QToolButton *>()) {
            if (b->toolTip().startsWith(QStringLiteral("尺子")))
                ruler = b;
        }
        check(ruler != nullptr, "ruler: toolbar button");
        if (ruler)
            ruler->click();
        drag(&overlay, {100, 150}, {250, 150});
        QTest::keyClick(&overlay, Qt::Key_Return);
        copied.save(out.filePath("ruler.png"));
        int red = 0;
        for (int y = qRound(110 * dpr) - 40; y < qRound(110 * dpr) + 40; ++y)
            for (int x = qRound(150 * dpr) - 40; x < qRound(150 * dpr) + 40; ++x)
                red += copied.valid(x, y) && copied.pixelColor(x, y) == QColor("#FF3B30");
        std::printf("      red pixels around the ruler's middle: %d\n", red);
        check(copied.pixelColor(QPoint(120, 110) * dpr) == QColor("#FF3B30"), "ruler: the measuring line is drawn");
        check(red > 600, "ruler: a length label sits by the line");
    }

    {
        SelectionOverlay overlay(screen, QPixmap::fromImage(page), {});
        connectCopy(overlay);
        overlay.show();
        QTest::qWaitForWindowExposed(&overlay);
        QTest::keyClick(&overlay, Qt::Key_F);
        // A triangle: (100,100) -> (300,100) -> (200,300) -> back.
        const QList<QPoint> corners = {{100, 100}, {300, 100}, {200, 300}, {100, 100}};
        QTest::mousePress(&overlay, Qt::LeftButton, {}, corners[0]);
        for (int c = 1; c < corners.size(); ++c)
            for (int i = 1; i <= 10; ++i)
                QTest::mouseMove(&overlay, corners[c - 1] + (corners[c] - corners[c - 1]) * i / 10);
        QTest::mouseRelease(&overlay, Qt::LeftButton, {}, corners.last());
        overlay.grab().save(out.filePath("freeform_overlay.png"));
        QTest::keyClick(&overlay, Qt::Key_Return);
        copied.save(out.filePath("freeform.png"));
        std::printf("      freeform image %dx%d, alpha %d\n", copied.width(), copied.height(), copied.hasAlphaChannel());
        check(qAbs(copied.width() - 200 * dpr) <= 2 && qAbs(copied.height() - 200 * dpr) <= 2,
              "freeform: the image is the shape's bounding box");
        check(copied.pixelColor(QPoint(100, 30) * dpr).alpha() == 255, "freeform: inside the shape is kept");
        check(copied.pixelColor(QPoint(10, 190) * dpr).alpha() == 0 && copied.pixelColor(QPoint(190, 190) * dpr).alpha() == 0,
              "freeform: outside the shape is transparent");
        if (QToolButton *spotlight = spotlightButton(overlay)) {
            spotlight->click();
            drag(&overlay, {170, 125}, {230, 170});
            QTest::keyClick(&overlay, Qt::Key_Return);
            const QColor dimmed = copied.pixelColor(QPoint(100, 100) * dpr);
            check(copied.pixelColor(QPoint(100, 50) * dpr) == Qt::white && dimmed.alpha() == 255
                      && dimmed.lightness() < 220 && copied.pixelColor(QPoint(10, 190) * dpr).alpha() == 0,
                  "spotlight: freeform export darkens the crop and preserves its transparent corners");
        }
    }
}

// Beautify: background, margin, rounded corners, shadow.
void testBeautify(const QDir &out)
{
    QImage source(200, 120, QImage::Format_RGB32);
    source.fill(QColor(255, 0, 0));
    Beautify::Options options;
    options.background = 1; // 海蓝渐变 #4facfe -> #00f2fe
    options.padding = 40;
    options.radius = 20;
    options.shadow = true;
    const QImage result = Beautify::render(source, options);
    check(result.size() == QSize(280, 200), "beautify: image plus the margin on every side");
    const QColor corner = result.pixelColor(1, 1);
    check(qAbs(corner.red() - 0x4f) < 6 && qAbs(corner.green() - 0xac) < 6 && corner.alpha() == 255,
          "beautify: the margin shows the chosen gradient");
    check(result.pixelColor(140, 100) == QColor(255, 0, 0), "beautify: the image itself is untouched");
    check(result.pixelColor(41, 41) != QColor(255, 0, 0), "beautify: corners are rounded off");

    options.background = 0; // transparent
    options.shadow = false;
    const QImage clear = Beautify::render(source, options);
    check(clear.pixelColor(1, 1).alpha() == 0 && clear.pixelColor(41, 41).alpha() == 0,
          "beautify: transparent background leaves margin and corners clear");

    QImage dot(1, 1, QImage::Format_RGB32);
    dot.fill(Qt::red);
    check(Beautify::render(dot, options).size() == QSize(81, 81), "beautify: a 1x1 image still renders");

    BeautifyDialog dialog(makeShot(QSize(480, 300)));
    Beautify::Options look;
    look.background = 2;
    dialog.setOptions(look);
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog) && dialog.result().width() == 480 + 2 * look.padding,
          "beautify: the window renders the preview");
    dialog.grab().save(out.filePath("beautify_window.png"));
    auto *backgroundBox = dialog.findChild<QComboBox *>();
    check(backgroundBox && backgroundBox->currentIndex() == look.background, "beautify: controls show the options in use");
}

// Full screen / active window: the region starts selected and Enter takes it as is.
void testPreselect()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    SelectionOverlay overlay(screen, makeShot(screen->geometry().size() * dpr), {});
    QImage copied;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction, const QPixmap &image, const QPoint &) { copied = image.toImage(); });
    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "preselect: overlay exposed");
    const QRect window(QPoint(50, 60) * dpr, QSize(300, 200) * dpr);
    overlay.preselect(window);
    QTest::keyClick(&overlay, Qt::Key_Return);
    check(copied.size() == window.size(), "preselect: Enter copies exactly the preselected window");
    overlay.preselect(QRect(QPoint(0, 0), screen->geometry().size() * dpr));
    QTest::keyClick(&overlay, Qt::Key_Return);
    check(copied.size() == screen->geometry().size() * dpr, "preselect: full screen");
}

void testCaptureToolbarLayout(const QDir &out)
{
    for (QScreen *screen : QGuiApplication::screens()) {
        const qreal dpr = screen->devicePixelRatio();
        const QRect geometry = screen->geometry();
        std::printf("      toolbar screen: %dx%d at (%d, %d), scale %.2f\n", geometry.width(), geometry.height(),
                    geometry.x(), geometry.y(), double(dpr));
        SelectionOverlay overlay(screen, makeShot(geometry.size() * dpr), {});
        overlay.show();
        check(QTest::qWaitForWindowExposed(&overlay), "toolbar: overlay exposed");
        auto *toolbar = overlay.findChild<CaptureToolbar *>();
        if (!toolbar) {
            check(false, "toolbar: capture controls exist");
            continue;
        }

        auto preselect = [&](const QRect &logical) {
            overlay.preselect(QRect(logical.topLeft() * dpr, logical.size() * dpr));
            QCoreApplication::processEvents();
        };
        preselect(QRect(24, 24, qMin(250, overlay.width() - 48), 180));
        auto *undo = toolbarButton(*toolbar, QStringLiteral("Ctrl+Z"));
        auto *ocr = toolbarButton(*toolbar, QStringLiteral("Ctrl+O"));
        auto *rectangle = toolbarButton(*toolbar, QStringLiteral("Ctrl+1"));
        auto *swatch = toolbarButton(*toolbar, QStringLiteral("#FF3B30"));
        if (undo && ocr && rectangle && swatch) {
            const auto top = [&](const QWidget *control) { return control->mapTo(toolbar, QPoint()).y(); };
            check(top(undo) + undo->height() <= top(ocr), "toolbar: image output starts below annotation history");
            check(!swatch->isVisible(), "toolbar: drawing options start hidden");
            selectTool(&overlay, 1);
            QCoreApplication::processEvents();
            check(swatch->isVisible() && top(swatch) >= top(undo) + undo->height()
                      && top(swatch) + swatch->height() <= top(ocr),
                  "toolbar: active drawing options sit directly below the annotation tools");
            selectTool(&overlay, 1);
        } else {
            check(false, "toolbar: annotation, history, style and output controls exist");
        }
        if (overlay.width() >= 800) {
            const QRect leftStrip(0, 0, 80, overlay.height());
            preselect(leftStrip);
            check(overlay.rect().contains(toolbar->geometry()) && toolbar->geometry().left() > leftStrip.right(),
                  "toolbar: uses free space beside a full-height selection");
        }

        overlay.resize(qMin(330, overlay.width()), qMin(560, overlay.height()));
        const QRect narrowStrip(0, 0, 80, overlay.height());
        preselect(narrowStrip);
        check(overlay.rect().contains(toolbar->geometry()) && toolbar->geometry().left() > narrowStrip.right(),
              "toolbar: wraps to fit beside a selection on a narrow display");

        preselect(overlay.rect());
        check(toolbar->isVisible() && overlay.rect().contains(toolbar->geometry()),
              "toolbar: fits a narrow portrait display");

        overlay.resize(qMin(190, overlay.width()), overlay.height());
        const QRect full = overlay.rect();
        const QRect bottomRight(full.width() - 120, full.height() - 90, 120, 90);
        preselect(bottomRight);
        selectTool(&overlay, 1);
        QCoreApplication::processEvents();
        check(full.contains(toolbar->geometry()) && toolbar->geometry().bottom() < bottomRight.top(),
              "toolbar: stays above a bottom-right selection while options are open");

        bool controlsReachable = true;
        for (QToolButton *button : toolbar->findChildren<QToolButton *>()) {
            const QRect buttonRect(button->mapTo(toolbar, QPoint()), button->size());
            controlsReachable &= button->isVisibleTo(toolbar) && toolbar->rect().contains(buttonRect)
                                 && full.contains(buttonRect.translated(toolbar->pos()))
                                 && buttonRect.width() >= 30 && buttonRect.height() >= 30
                                 && toolbar->childAt(buttonRect.center()) == button;
        }
        check(controlsReachable, "toolbar: every command and style control has a 30 px clickable area");
        overlay.grab().save(out.filePath(QStringLiteral("toolbar_%1x.png").arg(dpr)));
    }
}

void testCaptureToolbarKeyboard()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    SelectionOverlay overlay(screen, makeShot(screen->geometry().size() * dpr), {});
    const QRect selection(QPoint(60, 70) * dpr, QSize(300, 160) * dpr);
    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "toolbar keyboard: overlay exposed");
    overlay.preselect(selection);

    auto *toolbar = overlay.findChild<CaptureToolbar *>();
    auto *rectangle = toolbar ? toolbarButton(*toolbar, QStringLiteral("Ctrl+1")) : nullptr;
    overlay.activateWindow();
    overlay.setFocus();
    QTest::keyClick(&overlay, Qt::Key_Tab);
    check(rectangle && rectangle->hasFocus(), "toolbar keyboard: Tab reaches the annotation tools");
    if (!rectangle || !rectangle->hasFocus())
        return;

    QTest::keyClick(rectangle, Qt::Key_Space);
    check(toolbar->tool() == Tool::Rect && rectangle->isChecked(), "toolbar keyboard: Space activates a focused tool");
    QPoint copiedOrigin;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction, const QPixmap &, const QPoint &origin) { copiedOrigin = origin; });
    QTest::keyClick(rectangle, Qt::Key_Right);
    QTest::keyClick(rectangle, Qt::Key_Return);
    check(copiedOrigin == (selection.topLeft() + QPoint(1, 0)) / dpr + screen->geometry().topLeft(),
          "toolbar keyboard: the focused button preserves selection movement and Enter confirmation");
}

// Annotating a pinned image: the region is fixed and "done" hands back exactly that region.
void testFixedRegion(const QDir &out)
{
    QSettings().clear(); // red again, whatever colour the previous test picked
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    SelectionOverlay overlay(screen, makeShot(screen->geometry().size() * dpr), {});
    const QRect region(QPoint(100, 120) * dpr, QSize(240, 160) * dpr);
    overlay.editFixedRegion(region);
    QList<std::pair<CaptureAction, QImage>> results;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction action, const QPixmap &image, const QPoint &) {
                         results.append({action, image.toImage()});
                     });
    QSignalSpy cancelled(&overlay, &SelectionOverlay::cancelled);
    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "pin editing: overlay exposed");

    // Dragging outside with no tool must not start a new selection or move the region.
    drag(&overlay, {400, 400}, {600, 500});
    drag(&overlay, {150, 150}, {250, 250});
    selectTool(&overlay, 1);
    drag(&overlay, {120, 140}, {200, 200}); // a mark inside the image
    QTest::keyClick(&overlay, Qt::Key_Return);
    overlay.grab().save(out.filePath("m2_5_pin_editing.png"));
    check(results.size() == 1 && results.first().first == CaptureAction::Pin
              && results.first().second.size() == region.size(),
          "pin editing: Enter returns the fixed region (same size) as the pin's new image");
    check(!results.isEmpty() && results.first().second.pixelColor(QPoint(20, 40) * dpr) == QColor("#FF3B30"),
          "pin editing: the result carries the annotation");

    QTest::keyClick(&overlay, Qt::Key_S, Qt::ControlModifier); // capture-only actions do nothing here
    check(results.size() == 1, "pin editing: save/OCR/record shortcuts are disabled");
    selectTool(&overlay, 1); // tool off
    QTest::mouseClick(&overlay, Qt::RightButton, {}, {500, 500});
    check(cancelled.size() == 1, "pin editing: right-click without a tool cancels");

    QToolButton *spotlight = spotlightButton(overlay);
    check(spotlight && spotlight->isVisible(), "spotlight: toolbar button is available when editing a pin");
    auto *toolbar = overlay.findChild<CaptureToolbar *>();
    auto *done = toolbar ? toolbarButton(*toolbar, QStringLiteral("完成标注")) : nullptr;
    auto *save = toolbar ? toolbarButton(*toolbar, QStringLiteral("Ctrl+Shift+S")) : nullptr;
    auto *ocr = toolbar ? toolbarButton(*toolbar, QStringLiteral("Ctrl+O")) : nullptr;
    check(done && done->isVisible() && save && !save->isVisible() && ocr && !ocr->isVisible(),
          "pin editing: keeps the confirmation button and hides capture-only output");
    if (spotlight && done && !results.isEmpty()) {
        const QImage original = results.first().second;
        const QPoint inside = QPoint(55, 55) * dpr;
        const QPoint outside = QPoint(210, 130) * dpr;
        spotlight->click();
        drag(&overlay, {140, 160}, {185, 195});
        QTest::mouseClick(done, Qt::LeftButton);
        check(results.size() == 2 && results.last().first == CaptureAction::Pin
                  && results.last().second.pixelColor(inside) == original.pixelColor(inside)
                  && results.last().second.pixelColor(outside).lightness() < original.pixelColor(outside).lightness(),
              "spotlight: applying pin edits returns the spotlighted image");
    }
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts"); // the offscreen platform ships no fonts
    QCoreApplication::addLibraryPath(QStringLiteral(JIEYING_QT_PLUGIN_DIR));
    QApplication app(argc, argv);
    QApplication::setOrganizationName("JieyingTests"); // keep test settings away from the real app's
    QApplication::setApplicationName("overlay_test");
    QSettings().clear(); // default colour (red) and size, whatever an earlier run picked
    const QDir out(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath());

    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    SelectionOverlay overlay(screen, makeShot(screen->geometry().size() * dpr), {});

    QPixmap result;
    int copies = 0;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction action, const QPixmap &image, const QPoint &) {
                         if (action == CaptureAction::Copy) {
                             result = image;
                             ++copies;
                         }
                     });
    auto copy = [&] {
        QTest::keyClick(&overlay, Qt::Key_Return);
        return result.toImage();
    };

    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "overlay exposed");

    const QPoint origin(40, 40);
    drag(&overlay, origin, {640, 440});

    selectTool(&overlay, 1);
    drag(&overlay, {60, 60}, {200, 150});
    selectTool(&overlay, 2);
    drag(&overlay, {230, 60}, {370, 150});
    selectTool(&overlay, 3);
    drag(&overlay, {400, 150}, {560, 60});
    selectTool(&overlay, 4);
    QTest::mousePress(&overlay, Qt::LeftButton, {}, {60, 230});
    for (int i = 0; i <= 40; ++i)
        QTest::mouseMove(&overlay, QPoint(60 + 4 * i, 230 + int(25 * std::sin(i / 4.0))));
    QTest::mouseRelease(&overlay, Qt::LeftButton, {}, {220, 230});
    selectTool(&overlay, 6);
    drag(&overlay, {50, 395}, {235, 435}); // over the background text
    selectTool(&overlay, 7);
    for (int x : {460, 520, 580})
        QTest::mouseClick(&overlay, Qt::LeftButton, {}, {x, 230});

    selectTool(&overlay, 5);
    QTest::mouseClick(&overlay, Qt::LeftButton, {}, {60, 330});
    auto *edit = overlay.findChild<QPlainTextEdit *>();
    check(edit != nullptr, "text tool opens an inline editor");
    if (edit) {
        QTest::keyClicks(edit, "Hello Jietu");
        edit->insertPlainText(QStringLiteral(" 截图\nsecond line"));
        overlay.grab().save(out.filePath("m2_1_editing.png"));
        QTest::keyClick(edit, Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(overlay.findChild<QPlainTextEdit *>() == nullptr, "Esc commits and closes the editor");
    }

    // Undo/redo with a fourth number marker; sample inside its disc, beside the digit.
    selectTool(&overlay, 7);
    const QPoint marker(620, 230);
    QTest::mouseClick(&overlay, Qt::LeftButton, {}, marker);
    const QPoint probe = (marker - origin + QPoint(10, 0)) * dpr;
    const QColor red("#FF3B30");
    check(copy().pixelColor(probe) == red, "number marker drawn in the default color");
    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);
    check(copy().pixelColor(probe) != red, "Ctrl+Z removes the last annotation");
    QTest::keyClick(&overlay, Qt::Key_Y, Qt::ControlModifier);
    check(copy().pixelColor(probe) == red, "Ctrl+Y restores it");
    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);

    // Mosaic cells are flat, so a row only changes color at cell borders (~10 px apart),
    // where the gradient-and-text background changes almost every pixel.
    const QImage composed = copy();
    auto colorChanges = [](const QImage &image, int y, int x0, int x1) {
        int changes = 0;
        for (int x = x0 + 1; x <= x1; ++x)
            changes += image.pixel(x, y) != image.pixel(x - 1, y);
        return changes;
    };
    const int row = (410 - origin.y()) * qRound(dpr);
    const int x0 = (60 - origin.x()) * qRound(dpr);
    const int x1 = (220 - origin.x()) * qRound(dpr);
    const int mosaicChanges = colorChanges(composed, row, x0, x1);
    const int originalChanges = colorChanges(overlay.grab().toImage(), 0, x0, x1); // unannotated top row
    std::printf("      mosaic row: %d color changes, plain row: %d\n", mosaicChanges, originalChanges);
    check(mosaicChanges <= (x1 - x0) / 9 + 1 && originalChanges > mosaicChanges * 2, "mosaic pixelates");

    check(composed.size() == QSize(601, 401) * dpr, "composed image matches the 601x401 selection");
    check(copies == 4, "each Enter emits one copy action");
    overlay.grab().save(out.filePath("m2_2_overlay.png"));
    composed.save(out.filePath("m2_3_composed.png"));

    testEditing(out);
    testFixedRegion(out);
    testPreselect();
    testCaptureToolbarLayout(out);
    testCaptureToolbarKeyboard();
    testBeautify(out);
    testMoreTools(out);
    testSpotlight();
    testBorrowedKeys();
    testPinPositionLock();
    testPins(out);
    testPinGroups();
    testRestoreClosedPin();
    testRestoreAfterClosingAllPins();
    testRulerAndFreeform(out);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
