// Headless UI test: drives SelectionOverlay on Qt's offscreen platform with synthetic
// events and saves what it renders. Usage: overlay_test [output-dir]
#include "beautify.h"
#include "beautifydialog.h"
#include "imageio.h"
#include "pinboard.h"
#include "pinwindow.h"
#include "selectionoverlay.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QUrl>
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
    testBeautify(out);
    testMoreTools(out);
    testBorrowedKeys();
    testPins(out);
    testPinGroups();
    testRulerAndFreeform(out);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
