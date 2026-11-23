#include "livepindialog.h"
#include "windowfinder.h"
#include "wgcgrabber.h"

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <windows.h>

#include <utility>

namespace {

constexpr auto kTitlePrefix = L"Jieying live pin test ";
constexpr auto kFixtureClass = L"JieyingLivePinTestWindow";

LRESULT CALLBACK fixtureProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        const HBRUSH fill = CreateSolidBrush(RGB(207, 32, 163));
        FillRect(dc, &paint.rcPaint, fill);
        DeleteObject(fill);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int showFixture(const wchar_t *title)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = fixtureProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kFixtureClass;
    if (!RegisterClassW(&wc))
        return 1;

    const HWND window = CreateWindowExW(0, kFixtureClass, title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 260, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window)
        return 1;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

class Fixture
{
public:
    Fixture()
        : title(QString::fromWCharArray(kTitlePrefix) + QString::number(GetCurrentProcessId()))
    {
        process.start(QCoreApplication::applicationFilePath(), {"--live-pin-fixture", title});
    }

    ~Fixture()
    {
        close();
        if (!process.waitForFinished(1000) && process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
    }

    HWND window() const
    {
        return FindWindowW(kFixtureClass, reinterpret_cast<const wchar_t *>(title.utf16()));
    }

    void close()
    {
        if (auto hwnd = window())
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    const QString title;
    QProcess process;
};

CaptureWindow findWindow(const QString &title)
{
    for (const auto &window : snapshotCaptureWindows()) {
        if (window.title == title)
            return window;
    }
    return {};
}

class TestSource : public LivePinSource
{
public:
    bool isSupported() override { return supported; }
    QList<CaptureWindow> windows() override { return {{9, 7, 8, QStringLiteral("Test window")}}; }
    CaptureWindowState windowState(const CaptureWindow &) override { return state; }
    bool start(const CaptureWindow &) override
    {
        running = startSucceeds;
        return running;
    }
    WindowCaptureFrame poll() override { return std::exchange(next, {}); }
    void stop() override { running = false; next = {}; }
    QString error() const override { return QStringLiteral("Test capture failure"); }

    void frame(QSize size)
    {
        QImage image(size, QImage::Format_RGB32);
        image.fill(Qt::magenta);
        next = {WindowCaptureFrame::Status::Frame, image};
    }

    bool supported = true;
    bool startSucceeds = true;
    bool running = false;
    CaptureWindowState state = CaptureWindowState::Available;
    WindowCaptureFrame next;
};

QPushButton *button(LivePinDialog &dialog, const char *name)
{
    return dialog.findChild<QPushButton *>(QString::fromLatin1(name));
}

} // namespace

class LivePinTest : public QObject
{
    Q_OBJECT

private slots:
    void startsWithoutAnAvailableFrame();
    void freezesTheFullFrameAtThePaintedPosition();
    void disablesFreezeUntilFreshFrameAfterRestore();
    void stopsAndReportsInvalidOrFailedCapture();
    void listsForeignVisibleWindowsAndTracksTheirState();
    void reportsSelectionThatClosedBeforeStart();
    void previewsAcrossOcclusionAndResizeAndFreezesOriginal();
    void pausesOnMinimizeAndRecoversThenStopsOnClose();
};

void LivePinTest::startsWithoutAnAvailableFrame()
{
    LivePinDialog dialog;
    QVERIFY(!dialog.isModal());
    QVERIFY(dialog.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    const auto freeze = dialog.findChild<QPushButton *>("livePinFreeze");
    QVERIFY(freeze);
    QVERIFY(!freeze->isEnabled());
}

void LivePinTest::freezesTheFullFrameAtThePaintedPosition()
{
    auto backend = std::make_unique<TestSource>();
    auto *source = backend.get();
    LivePinDialog dialog(std::move(backend), nullptr);
    dialog.resize(460, 440);
    dialog.show();
    const auto start = button(dialog, "livePinStart");
    const auto freeze = button(dialog, "livePinFreeze");
    const auto preview = dialog.findChild<QWidget *>("livePinPreview");
    QVERIFY(start && freeze && preview);
    start->click();
    source->frame(QSize(1200, 400));
    QTRY_VERIFY(freeze->isEnabled());
    const auto rendered = preview->grab().toImage();
    QCOMPARE(rendered.pixelColor(rendered.rect().center()), QColor(Qt::magenta));
    QVERIFY(rendered.pixelColor(rendered.width() / 2, 0) != QColor(Qt::magenta));

    QSignalSpy frozen(&dialog, &LivePinDialog::freezeRequested);
    freeze->click();
    QCOMPARE(frozen.count(), 1);
    QVERIFY(!dialog.isVisible());
    QVERIFY(!source->running);
    const auto pixmap = frozen.front().front().value<QPixmap>();
    QCOMPARE(pixmap.size(), QSize(1200, 400));
    QCOMPARE(pixmap.devicePixelRatio(), preview->devicePixelRatioF());
    QCOMPARE(pixmap.toImage().pixelColor(15, 15), QColor(Qt::magenta));
    const auto imageOrigin = preview->mapFromGlobal(frozen.front().at(1).value<QPoint>());
    QCOMPARE(imageOrigin.x(), 0);
    QVERIFY(imageOrigin.y() > 0);
    const qreal scale = frozen.front().at(2).toReal();
    QVERIFY(qAbs(pixmap.deviceIndependentSize().width() * scale - preview->width()) <= 3);
}

void LivePinTest::disablesFreezeUntilFreshFrameAfterRestore()
{
    auto backend = std::make_unique<TestSource>();
    auto *source = backend.get();
    LivePinDialog dialog(std::move(backend), nullptr);
    dialog.show();
    const auto freeze = button(dialog, "livePinFreeze");
    button(dialog, "livePinStart")->click();
    source->frame(QSize(500, 400));
    QTRY_VERIFY(freeze->isEnabled());
    QSignalSpy frozen(&dialog, &LivePinDialog::freezeRequested);
    source->state = CaptureWindowState::Minimized;
    freeze->click();
    QVERIFY(frozen.isEmpty());
    QVERIFY(!freeze->isEnabled());
    QVERIFY(!source->running);
    source->state = CaptureWindowState::Available;
    QTRY_VERIFY(source->running);
    QVERIFY(!freeze->isEnabled());
    source->frame(QSize(400, 500));
    QTRY_VERIFY(freeze->isEnabled());
    source->state = CaptureWindowState::Unavailable;
    QTRY_VERIFY(!freeze->isEnabled());
    QVERIFY(!source->running);
    source->state = CaptureWindowState::Available;
    QTRY_VERIFY(source->running);
    QVERIFY(!freeze->isEnabled());
    source->frame(QSize(400, 500));
    QTRY_VERIFY(freeze->isEnabled());
    source->next = {WindowCaptureFrame::Status::Resizing, {}};
    QTRY_VERIFY(!freeze->isEnabled());
    source->frame(QSize(300, 900));
    QTRY_VERIFY(freeze->isEnabled());
    source->next = {WindowCaptureFrame::Status::Closed, {}};
    QTRY_VERIFY(!freeze->isEnabled());
    QVERIFY(!source->running);
}

void LivePinTest::stopsAndReportsInvalidOrFailedCapture()
{
    auto backend = std::make_unique<TestSource>();
    auto *source = backend.get();
    LivePinDialog dialog(std::move(backend), nullptr);
    dialog.show();
    const auto status = dialog.findChild<QLabel *>("livePinStatus");
    QVERIFY(status);
    source->state = CaptureWindowState::Closed;
    button(dialog, "livePinStart")->click();
    QVERIFY(status->text().contains(QStringLiteral("已关闭")));
    button(dialog, "livePinChoose")->click();
    source->state = CaptureWindowState::Available;
    source->startSucceeds = false;
    button(dialog, "livePinStart")->click();
    QVERIFY(status->text().contains(QStringLiteral("无法捕获")));
    QVERIFY(!button(dialog, "livePinFreeze")->isEnabled());
    QVERIFY(!source->running);

    button(dialog, "livePinChoose")->click();
    source->startSucceeds = true;
    button(dialog, "livePinStart")->click();
    source->next = {WindowCaptureFrame::Status::Error, {}};
    QTRY_VERIFY(!source->running);
    QVERIFY(status->text().contains(QStringLiteral("无法继续捕获")));
    QCOMPARE(status->toolTip(), source->error());
}

void LivePinTest::listsForeignVisibleWindowsAndTracksTheirState()
{
    if (qEnvironmentVariableIntValue("JIEYING_TEST_LIVE_CAPTURE") != 1)
        QSKIP("Set JIEYING_TEST_LIVE_CAPTURE=1 for native window tests");
    Fixture source;
    QVERIFY(source.process.waitForStarted());
    QTRY_VERIFY(source.window());
    QTRY_VERIFY(findWindow(source.title).handle != 0);
    const auto selected = findWindow(source.title);
    QCOMPARE(captureWindowState(selected), CaptureWindowState::Available);
    auto differentThread = selected;
    ++differentThread.threadId;
    QCOMPARE(captureWindowState(differentThread), CaptureWindowState::Closed);

    QWidget ownWindow;
    ownWindow.setWindowTitle(QStringLiteral("Own live pin test tool window"));
    ownWindow.show();
    QCOMPARE(findWindow(ownWindow.windowTitle()).handle, quintptr(0));

    QVERIFY(ShowWindowAsync(source.window(), SW_MINIMIZE));
    QTRY_COMPARE(captureWindowState(selected), CaptureWindowState::Minimized);
    QCOMPARE(findWindow(source.title).handle, quintptr(0));
    QVERIFY(ShowWindowAsync(source.window(), SW_SHOWNOACTIVATE));
    QTRY_COMPARE(captureWindowState(selected), CaptureWindowState::Available);
    QVERIFY(ShowWindowAsync(source.window(), SW_HIDE));
    QTRY_COMPARE(captureWindowState(selected), CaptureWindowState::Unavailable);
    source.close();
    QTRY_COMPARE(captureWindowState(selected), CaptureWindowState::Closed);
}

void LivePinTest::reportsSelectionThatClosedBeforeStart()
{
    if (qEnvironmentVariableIntValue("JIEYING_TEST_LIVE_CAPTURE") != 1)
        QSKIP("Set JIEYING_TEST_LIVE_CAPTURE=1 for native window tests");
    if (!WgcWindowGrabber::isSupported())
        QSKIP("Windows Graphics Capture is unavailable");
    Fixture source;
    QVERIFY(source.process.waitForStarted());
    QTRY_VERIFY(findWindow(source.title).handle != 0);
    LivePinDialog dialog;
    dialog.show();
    const auto list = dialog.findChild<QListWidget *>("livePinWindows");
    const auto start = dialog.findChild<QPushButton *>("livePinStart");
    const auto status = dialog.findChild<QLabel *>("livePinStatus");
    QVERIFY(list && start && status);
    const auto entries = list->findItems(source.title, Qt::MatchExactly);
    QCOMPARE(entries.size(), 1);
    list->setCurrentItem(entries.front());
    QVERIFY(start->isEnabled());
    source.close();
    QTRY_VERIFY(!source.window());
    start->click();
    QVERIFY(status->text().contains(QStringLiteral("已关闭")));
    QVERIFY(!dialog.findChild<QPushButton *>("livePinFreeze")->isEnabled());
}

void LivePinTest::previewsAcrossOcclusionAndResizeAndFreezesOriginal()
{
    if (QApplication::platformName() != QStringLiteral("windows")
        || !WgcWindowGrabber::isSupported())
        QSKIP("Requires the Windows desktop and Windows Graphics Capture");
    Fixture source;
    QVERIFY(source.process.waitForStarted());
    QTRY_VERIFY(findWindow(source.title).handle != 0);
    const HWND target = source.window();
    RECT bounds{};
    QVERIFY(GetWindowRect(target, &bounds));
    QWidget cover(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
    cover.setAutoFillBackground(true);
    auto palette = cover.palette();
    palette.setColor(QPalette::Window, Qt::green);
    cover.setPalette(palette);
    // Native coordinates keep the cover aligned with the fixture on HiDPI displays.
    cover.show();
    QVERIFY(SetWindowPos(reinterpret_cast<HWND>(cover.winId()), HWND_TOPMOST,
                        bounds.left, bounds.top, bounds.right - bounds.left,
                        bounds.bottom - bounds.top, SWP_NOACTIVATE));
    LivePinDialog dialog;
    dialog.show();
    const auto list = dialog.findChild<QListWidget *>("livePinWindows");
    const auto start = dialog.findChild<QPushButton *>("livePinStart");
    const auto freeze = dialog.findChild<QPushButton *>("livePinFreeze");
    const auto preview = dialog.findChild<QWidget *>("livePinPreview");
    const auto status = dialog.findChild<QLabel *>("livePinStatus");
    QVERIFY(list && start && freeze && preview && status);
    const auto entries = list->findItems(source.title, Qt::MatchExactly);
    QCOMPARE(entries.size(), 1);
    list->setCurrentItem(entries.front());
    start->click();
    QTRY_VERIFY2(freeze->isEnabled(), qPrintable(status->text() + " " + status->toolTip()));
    const QColor fixtureColor(207, 32, 163);
    auto rendered = [&] { return preview->grab().toImage(); };
    auto initial = rendered();
    QCOMPARE(initial.pixelColor(initial.rect().center()), fixtureColor);
    QVERIFY(initial.pixelColor(initial.width() / 2, initial.height() - 1) != fixtureColor);

    QVERIFY(SetWindowPos(target, nullptr, 0, 0, 350, 500,
                         SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE));
    QTRY_VERIFY(rendered().pixelColor(rendered().width() / 2, rendered().height() * 3 / 4)
                == fixtureColor);
    QSignalSpy frozen(&dialog, &LivePinDialog::freezeRequested);
    freeze->click();
    QCOMPARE(frozen.count(), 1);
    QVERIFY(!dialog.isVisible());
    const auto image = frozen.front().front().value<QPixmap>();
    QVERIFY(image.height() > image.width());
    QVERIFY(image.height() > 400);
    QCOMPARE(image.toImage().pixelColor(image.rect().center()), fixtureColor);
    const auto imageOrigin = frozen.front().at(1).value<QPoint>();
    QVERIFY(preview->rect().contains(preview->mapFromGlobal(imageOrigin)));
}

void LivePinTest::pausesOnMinimizeAndRecoversThenStopsOnClose()
{
    if (QApplication::platformName() != QStringLiteral("windows")
        || !WgcWindowGrabber::isSupported())
        QSKIP("Requires the Windows desktop and Windows Graphics Capture");
    Fixture source;
    QVERIFY(source.process.waitForStarted());
    QTRY_VERIFY(findWindow(source.title).handle != 0);
    LivePinDialog dialog;
    dialog.show();
    const auto list = dialog.findChild<QListWidget *>("livePinWindows");
    const auto start = dialog.findChild<QPushButton *>("livePinStart");
    const auto freeze = dialog.findChild<QPushButton *>("livePinFreeze");
    const auto status = dialog.findChild<QLabel *>("livePinStatus");
    QVERIFY(list && start && freeze && status);
    const auto entries = list->findItems(source.title, Qt::MatchExactly);
    QCOMPARE(entries.size(), 1);
    list->setCurrentItem(entries.front());
    start->click();
    QTRY_VERIFY2(freeze->isEnabled(), qPrintable(status->text() + " " + status->toolTip()));
    QVERIFY(ShowWindowAsync(source.window(), SW_MINIMIZE));
    QTRY_VERIFY(!freeze->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("已最小化")));
    QVERIFY(ShowWindowAsync(source.window(), SW_SHOWNOACTIVATE));
    QTRY_VERIFY2(freeze->isEnabled(), qPrintable(status->text() + " " + status->toolTip()));
    source.close();
    QTRY_VERIFY(!freeze->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("已关闭")));
}

int main(int argc, char *argv[])
{
    if (argc == 3 && QByteArrayView(argv[1]) == "--live-pin-fixture") {
        const auto title = QString::fromLocal8Bit(argv[2]);
        return showFixture(reinterpret_cast<const wchar_t *>(title.utf16()));
    }
#ifdef JIEYING_QT_PLUGIN_DIR
    qputenv("QT_PLUGIN_PATH", JIEYING_QT_PLUGIN_DIR);
#endif
    if (qEnvironmentVariableIntValue("JIEYING_TEST_LIVE_CAPTURE") != 1)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    LivePinTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "livepin_test.moc"
