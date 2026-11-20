// Headless OCR test: renders known text, recognizes it with Windows.Media.Ocr and checks
// the result window. Usage: ocr_test [output-dir]
#include "barcodes.h"
#include "ocr.h"
#include "ocrwindow.h"
#include "selectionoverlay.h"
#include "sensitive.h"
#ifdef JIEYING_HAVE_PADDLEOCR
#include "paddleocr.h"
#endif

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QtConcurrent>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

QString compact(QString s)
{
    s.remove(QLatin1Char(' '));
    return s;
}

// 1 - normalized Levenshtein distance.
qreal similarity(const QString &a, const QString &b)
{
    QList<int> row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            row[j] = qMin(qMin(row[j] + 1, row[j - 1] + 1), diagonal + (a[i - 1] == b[j - 1] ? 0 : 1));
            diagonal = above;
        }
    }
    return 1.0 - qreal(row[b.size()]) / qMax(1, int(qMax(a.size(), b.size())));
}

} // namespace

// A long screenshot: 900 x ~7000 px of numbered lines. Scaling it down whole would make the
// text unreadable, so recognition has to work on strips.
void testLongImage(const QDir &out, int kLines)
{
    QImage page(900, 40 + kLines * 58, QImage::Format_RGB32);
    page.fill(Qt::white);
    {
        QPainter p(&page);
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPixelSize(18);
        p.setFont(font);
        for (int i = 1; i <= kLines; ++i)
            p.drawText(30, 40 + i * 58 - 20, QStringLiteral("第%1行 长截图识别测试 line %1").arg(i));
    }
    page.save(out.filePath("ocr_long_source.png"));

    QElapsedTimer timer;
    timer.start();
    const OcrResult result = QtConcurrent::run([page] { return Ocr::recognize(page); }).result();
    int found = 0;
    for (int i = 1; i <= kLines; ++i) {
        const QString wanted = QStringLiteral("第%1行").arg(i);
        found += std::any_of(result.lines.cbegin(), result.lines.cend(),
                             [&](const OcrLine &line) { return line.text.startsWith(wanted); });
    }
    std::printf("      %s: %d of %d lines, %lld lines total, %lld ms\n", qPrintable(result.language), found, kLines,
                result.lines.size(), timer.elapsed());
    check(found >= kLines * 95 / 100, "long image: nearly every line of a 7000 px tall capture is read");
    check(result.lines.size() <= kLines + 2, "long image: lines in strip overlaps are not duplicated");
    const bool ordered = std::is_sorted(result.lines.cbegin(), result.lines.cend(),
                                        [](const OcrLine &a, const OcrLine &b) { return a.box.top() < b.box.top(); });
    check(ordered, "long image: lines come back top to bottom with page coordinates");
}

#ifdef JIEYING_HAVE_ZXING
// QR codes: generated here, placed into screenshots in different ways, read back.
void testBarcodes(const QDir &out)
{
    const QString url = QStringLiteral("https://gitee.com/Wang_yuan_com/jieying");
    const QString chinese = QStringLiteral("截影二维码测试 2026");

    QImage page(900, 520, QImage::Format_RGB32);
    page.fill(Qt::white);
    {
        QPainter p(&page);
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPixelSize(16);
        p.setFont(font);
        p.drawText(30, 40, QStringLiteral("扫码访问项目主页 Scan to open the project"));
        p.drawImage(QPoint(40, 70), Barcodes::makeQrCode(url, 6));

        // Light-on-dark, rotated a quarter turn and small: a dark-theme screenshot.
        QImage dark = Barcodes::makeQrCode(chinese, 3).convertToFormat(QImage::Format_RGB32);
        dark.invertPixels();
        dark = dark.transformed(QTransform().rotate(90));
        p.fillRect(QRect(470, 60, 400, 400), QColor(30, 30, 30));
        p.drawImage(QPoint(560, 150), dark);
    }
    page.save(out.filePath("qr_source.png"));

    const QList<Barcode> codes = QtConcurrent::run([page] { return Barcodes::detect(page); }).result();
    QStringList texts;
    for (const Barcode &code : codes)
        texts << code.text;
    std::printf("      found %lld: %s\n", codes.size(), texts.join(QStringLiteral(" | ")).toUtf8().constData());
    check(texts.contains(url), "qr: a URL code in a screenshot is read");
    check(texts.contains(chinese), "qr: an inverted, rotated, small code with Chinese text is read");
    check(!codes.isEmpty() && codes.first().corners.size() == 4, "qr: corner positions are reported");

    OcrWindow window(page);
    QSignalSpy found(&window, &OcrWindow::codesDetected);
    window.show();
    check(found.wait(20000) && window.codes().contains(url), "qr: the OCR window lists the codes it found");
    QSignalSpy recognized(&window, &OcrWindow::recognized);
    if (window.text().isEmpty())
        recognized.wait(20000);
    window.grab().save(out.filePath("qr_window.png"));
    std::printf("      window text: %s\n", window.text().replace(QLatin1Char('\n'), QStringLiteral(" / ")).toUtf8().constData());
    check(window.text().contains(QStringLiteral("扫码访问")) && !window.text().contains(QStringLiteral("回")),
          "qr: text read from inside the codes is dropped, real text kept");
}
#endif

// A one-pixel or one-row selection is easy to make by accident; every engine must cope.
void testTinyImages()
{
    const QSize sizes[] = {{1, 1}, {2, 2}, {300, 1}, {1, 300}, {7, 5000}, {5000, 7}};
    for (const QSize size : sizes) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(Qt::white);
        const QByteArray what = QStringLiteral("tiny %1x%2: OCR and code reading return without crashing")
                                    .arg(size.width())
                                    .arg(size.height())
                                    .toUtf8();
        bool ok = true;
        for (const Config::OcrEngine engine : {Config::OcrEngine::Auto, Config::OcrEngine::Windows}) {
            const OcrResult result = QtConcurrent::run([image, engine] { return Ocr::recognize(image, engine); }).result();
            ok = ok && result.lines.isEmpty();
            if (!result.error.isEmpty())
                std::printf("      engine %d: %s\n", int(engine), qPrintable(result.error));
        }
        ok = ok && Barcodes::detect(image).isEmpty();
        check(ok, what.constData());
    }
}

// Personal data rules. The ID and card numbers are made up, with valid check digits.
void testSensitiveRules()
{
    auto spans = [](const char *text) {
        const QString s = QString::fromUtf8(text);
        QStringList found;
        for (const Sensitive::Match &m : Sensitive::find(s))
            found << s.mid(m.start, m.length);
        return found;
    };
    check(spans("电话：13812345678") == QStringList{"13812345678"}, "sensitive: mobile number");
    check(spans("手机 +86 138 1234 5678 请联系") == QStringList{"+86 138 1234 5678"}, "sensitive: grouped mobile with +86");
    check(spans("座机 010-12345678") == QStringList{"010-12345678"}, "sensitive: landline with area code");
    check(spans("邮箱 test.user@example.com.") == QStringList{"test.user@example.com"}, "sensitive: e-mail");
    check(spans("身份证 110105199003071239") == QStringList{"110105199003071239"}, "sensitive: ID number");
    check(spans("编号 110105199003071230").isEmpty(), "sensitive: 18 digits with a wrong check digit are left alone");
    check(spans("卡号 6222 0212 3456 7890 128") == QStringList{"6222 0212 3456 7890 128"},
          "sensitive: grouped bank card (Luhn)");
    check(spans("卡号4111111111111111") == QStringList{"4111111111111111"}, "sensitive: bank card next to CJK");
    check(spans("订单号 1234567890123456").isEmpty(), "sensitive: long numbers failing Luhn are left alone");
    check(spans("服务器 192.168.1.10 已连接") == QStringList{"192.168.1.10"}, "sensitive: IPv4 address");
    check(spans("版本 1.2.3，价格 12345 元，时间 2026-10-30 12:30").isEmpty(), "sensitive: ordinary numbers ignored");

    // "电话" + 11 digits: 2 + 11 * 0.55 = 8.05 units over 805 px, so the digits start at 200 px.
    const QRect cover = Sensitive::locate({QStringLiteral("电话13812345678"), QRect(0, 0, 805, 100)}, 2, 11);
    std::printf("      located [%d..%d]\n", cover.left(), cover.right());
    check(cover.left() >= 160 && cover.left() <= 180 && cover.right() >= 804, "sensitive: span located within the line");
}

// One-click redaction in the capture overlay, with the real recognizer.
void testRedaction(const QDir &out)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen->devicePixelRatio();
    QImage page(screen->geometry().size() * dpr, QImage::Format_RGB32);
    page.fill(Qt::white);
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(qRound(18 * dpr));
    const QFontMetrics fm(font);
    const QString label = QStringLiteral("电话 ");
    const QString phone = QStringLiteral("13812345678");
    const QPoint phoneAt(qRound(60 * dpr), qRound(100 * dpr)); // baseline
    {
        QPainter p(&page);
        p.setFont(font);
        p.setPen(Qt::black);
        p.drawText(phoneAt, label + phone);
        p.drawText(QPoint(qRound(60 * dpr), qRound(150 * dpr)), QStringLiteral("邮箱 test.user@example.com"));
        p.drawText(QPoint(qRound(60 * dpr), qRound(200 * dpr)), QStringLiteral("普通文字不需要打码"));
    }
    const QRect labelBox(phoneAt.x(), phoneAt.y() - fm.ascent(), fm.horizontalAdvance(QStringLiteral("电话")), fm.height());
    const QRect phoneBox(phoneAt.x() + fm.horizontalAdvance(label), phoneAt.y() - fm.ascent(), fm.horizontalAdvance(phone),
                         fm.height());
    const QRect plainBox(qRound(60 * dpr), qRound(200 * dpr) - fm.ascent(),
                         fm.horizontalAdvance(QStringLiteral("普通文字不需要打码")), fm.height());

    SelectionOverlay overlay(screen, QPixmap::fromImage(page), {});
    QImage copied;
    QObject::connect(&overlay, &SelectionOverlay::actionTriggered,
                     [&](CaptureAction, const QPixmap &image, const QPoint &) { copied = image.toImage(); });
    QSignalSpy finished(&overlay, &SelectionOverlay::redactFinished);
    overlay.show();
    check(QTest::qWaitForWindowExposed(&overlay), "redact: overlay exposed");
    const QPoint origin(20, 20), corner(700, 300);
    QTest::mousePress(&overlay, Qt::LeftButton, {}, origin);
    for (int i = 1; i <= 8; ++i)
        QTest::mouseMove(&overlay, origin + (corner - origin) * i / 8);
    QTest::mouseRelease(&overlay, Qt::LeftButton, {}, corner);
    QTest::keyClick(&overlay, Qt::Key_D, Qt::ControlModifier);
    check(finished.wait(60'000) || !finished.isEmpty(), "redact: recognition finishes");
    const int count = finished.isEmpty() ? -1 : finished.first().first().toInt();
    std::printf("      %d region(s) redacted\n", count);
    check(count == 2, "redact: the phone number and the e-mail address are covered");

    QTest::keyClick(&overlay, Qt::Key_Return);
    copied.save(out.filePath("redact_result.png"));
    const QPoint offset = origin * dpr; // the copy starts at the selection's corner
    auto changed = [&](const QRect &box) { // share of pixels that differ from the original page
        int differ = 0;
        for (int y = box.top(); y <= box.bottom(); ++y)
            for (int x = box.left(); x <= box.right(); ++x)
                differ += copied.pixel(QPoint(x, y) - offset) != page.pixel(x, y);
        return qreal(differ) / (box.width() * box.height());
    };
    std::printf("      changed: label %.2f, phone %.2f, plain line %.2f\n", changed(labelBox), changed(phoneBox),
                changed(plainBox));
    check(changed(phoneBox) > 0.5, "redact: the digits are pixelated");
    check(changed(labelBox) < 0.25 && changed(plainBox) == 0, "redact: other text stays readable");

    QTest::keyClick(&overlay, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&overlay, Qt::Key_Return);
    check(changed(phoneBox) == 0, "redact: one Ctrl+Z removes all of it");
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    QCoreApplication::addLibraryPath(QStringLiteral(JIEYING_QT_PLUGIN_DIR));
    QApplication app(argc, argv);
    const QDir out(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath());

    // Boxes 20 px tall: a 1 px gap is touching, a 10 px gap is a visible space.
    auto word = [](const char *text, qreal left, qreal width) {
        return Ocr::Word{QString::fromUtf8(text), QRectF(left, 0, width, 20)};
    };
    check(Ocr::joinWords({word("截", 0, 20), word("图", 21, 20), word("Hello", 42, 50), word("World", 100, 50),
                          word("2026", 158, 40), word("年", 199, 20)})
              == QStringLiteral("截图Hello World 2026年"),
          "joinWords glues CJK, spaces Latin words, glues touching Latin/CJK");
    check(Ocr::joinWords({word("Wo", 0, 25), word("rld", 26, 30)}) == QStringLiteral("World"),
          "joinWords glues a Latin word the recognizer split");

    // Punctuation the recognizers mix up, fixed from context; correct text stays as it is.
    const std::pair<const char *, const char *> punctuation[] = {
        {"已读 23：05", "已读 23:05"},
        {"保存到 D ： \\Documents", "保存到 D:\\Documents"},
        {"14 ：30", "14:30"},
        {"下午 14： 30", "下午 14:30"},
        {"D： \\Documents", "D:\\Documents"},
        {"¥1， 286.50", "¥1,286.50"},
        {"文件在 D：\\Documents", "文件在 D:\\Documents"},
        {"总计 ¥1，286.50（含运费 ¥12.00)", "总计 ¥1,286.50（含运费 ¥12.00）"},
        {"Error (code 5）", "Error (code 5)"},
        {"上一页 下—页", "上一页 下一页"},
        {"npm install —save-dev", "npm install --save-dev"},
        {"Error Ox80070005", "Error 0x80070005"},
        {"10月3O日", "10月30日"},
        {"时间:10月30日,地点:3楼", "时间：10月30日，地点：3楼"},
        {"你好?", "你好？"},
        {"会议时间：10月30日，地点：3楼", "会议时间：10月30日，地点：3楼"},
        {"Hello, world: OK", "Hello, world: OK"},
        {"北京——上海", "北京——上海"},
        {"Windows安全:Defender", "Windows安全:Defender"},
    };
    bool fixedAll = true;
    for (const auto &[in, expected] : punctuation) {
        const QString got = Ocr::fixPunctuation(QString::fromUtf8(in));
        if (got != QString::fromUtf8(expected)) {
            std::printf("      fixPunctuation(%s) = %s, expected %s\n", in, got.toUtf8().constData(), expected);
            fixedAll = false;
        }
    }
    check(fixedAll, "fixPunctuation: half/full-width marks, brackets, dashes and O/0 follow their context");
    check(Ocr::joinWords({word("Jietu", 0, 60), word("截", 70, 20), word("图", 91, 20)}) == QStringLiteral("Jietu 截图"),
          "joinWords keeps a visible gap between Latin and CJK");

    // Exact text, spaces included. Avoids full-width punctuation like "：", which the
    // Windows recognizer reads inconsistently (seen as "．").
    const QStringList expected = {
        QStringLiteral("Jietu 截图工具 OCR 测试"),
        QStringLiteral("Hello World 2026"),
        QStringLiteral("第三行中文和English混排"),
    };
    // Default 15 px: typical UI text at 100% scaling, which is what screenshots mostly contain.
    const int requestedSize = qEnvironmentVariableIntValue("JIEYING_OCR_TEST_PX");
    const int pixelSize = requestedSize > 0 ? requestedSize : 15;
    std::printf("      font %d px\n", pixelSize);
    QImage image(pixelSize * 25, pixelSize * 8, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter p(&image);
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPixelSize(pixelSize);
        p.setFont(font);
        p.setPen(Qt::black);
        for (int i = 0; i < expected.size(); ++i)
            p.drawText(pixelSize, pixelSize * 2 + i * pixelSize * 2, expected[i]);
    }
    image.save(out.filePath("m3_0_source.png"));

    // Lines 1-2 are exact for Windows OCR: they exercise our word joining. Its line 3 (mixed
    // CJK/Latin) measures the recognizer itself, which misreads glyphs now and then.
    // PaddleOCR reads spaces itself and is held to exact text on every line.
    auto checkEngine = [&](const char *name, OcrResult (*engine)(const QImage &), bool exactAll) {
        std::printf("  -- %s\n", name);
        // WinRT's blocking get() must not run on the GUI thread, same as in the app.
        QElapsedTimer timer;
        timer.start();
        const OcrResult result = QtConcurrent::run([image, engine] { return engine(image); }).result();
        const qint64 firstMs = timer.restart(); // includes loading the engine/models
        QtConcurrent::run([image, engine] { return engine(image); }).waitForFinished();
        std::printf("      %s, first %lld ms, then %lld ms, error '%s'\n", qPrintable(result.language), firstMs,
                    timer.elapsed(), qPrintable(result.error));
        for (const OcrLine &line : result.lines)
            std::printf("      [%d,%d %dx%d] %s\n", line.box.x(), line.box.y(), line.box.width(), line.box.height(),
                        line.text.toUtf8().constData());
        check(result.error.isEmpty(), "recognition succeeds");
        check(result.lines.size() == expected.size(), "finds all three lines");
        for (int i = 0; i < qMin(result.lines.size(), expected.size()); ++i) {
            const qreal score = similarity(result.lines[i].text, expected[i]);
            const bool exact = exactAll || i < 2;
            const bool ok = exact ? result.lines[i].text == expected[i] : score >= 0.85;
            std::printf("%s  line %d text (%s, similarity %.0f%%)\n", ok ? "PASS" : "FAIL", i + 1,
                        exact ? "exact" : ">= 85%", score * 100);
            failures += ok ? 0 : 1;
        }
        if (!result.lines.isEmpty()) {
            const QRect first = result.lines.first().box;
            // The first line's glyphs sit between y = 2px - size and 2px + a little, starting at x = px.
            check(first.top() >= pixelSize / 2 && first.bottom() <= pixelSize * 3 && first.left() >= pixelSize / 2
                      && first.left() <= pixelSize * 2,
                  "line boxes are mapped back to source pixels (no scaling leak)");
        }
    };
    checkEngine("Windows OCR", &Ocr::recognizeWindows, false);
#ifdef JIEYING_HAVE_PADDLEOCR
    const bool paddle = PaddleOcr::isAvailable();
    check(paddle, "PaddleOCR runtime and models are deployed next to the exe");
    if (paddle) // without the DLL, calling in would fail the delay-load
        checkEngine("PaddleOCR", &PaddleOcr::recognize, true);
#endif

    // Harder sample: dark theme, full-width punctuation, symbols. Reported for both engines;
    // PaddleOCR must stay close (>= 90% per line).
    const QStringList darkLines = {
        QStringLiteral("设置：显示缩放 125%，分辨率 1920×1080"),
        QStringLiteral("错误码 0x80070005，拒绝访问。"),
        QStringLiteral("File Edit View Help"),
    };
    QImage dark(pixelSize * 28, pixelSize * 8, QImage::Format_RGB32);
    dark.fill(QColor(30, 30, 30));
    {
        QPainter p(&dark);
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPixelSize(pixelSize);
        p.setFont(font);
        p.setPen(QColor(212, 212, 212));
        for (int i = 0; i < darkLines.size(); ++i)
            p.drawText(pixelSize, pixelSize * 2 + i * pixelSize * 2, darkLines[i]);
    }
    dark.save(out.filePath("m3_2_dark_source.png"));
    auto reportDark = [&](const char *name, OcrResult (*engine)(const QImage &), bool required) {
        const OcrResult result = QtConcurrent::run([dark, engine] { return engine(dark); }).result();
        bool ok = result.lines.size() == darkLines.size();
        std::printf("  -- %s, dark theme\n", name);
        for (int i = 0; i < result.lines.size(); ++i) {
            const qreal score = i < darkLines.size() ? similarity(result.lines[i].text, darkLines[i]) : 0;
            ok = ok && score >= 0.9;
            std::printf("      %3.0f%%  %s\n", score * 100, result.lines[i].text.toUtf8().constData());
        }
        if (required)
            check(ok, "PaddleOCR reads the dark-theme sample (>= 90% per line)");
    };
    reportDark("Windows OCR", &Ocr::recognizeWindows, false);
#ifdef JIEYING_HAVE_PADDLEOCR
    if (paddle)
        reportDark("PaddleOCR", &PaddleOcr::recognize, true);
#endif

    // The window uses the default engine (PaddleOCR when available).
    const OcrResult result = QtConcurrent::run([image] { return Ocr::recognize(image); }).result();
    OcrWindow window(image);
    QSignalSpy recognized(&window, &OcrWindow::recognized);
    window.show();
    check(recognized.wait(20000), "result window finishes recognition");
    check(compact(window.text()) == compact(result.text()), "result window shows the recognized text");
    window.grab().save(out.filePath("m3_1_window.png"));
#ifdef JIEYING_HAVE_ZXING
    testBarcodes(out);
    testTinyImages();
    testSensitiveRules();
    testRedaction(out);
#endif
    testLongImage(out, 120);  // ~7000 px
    testLongImage(out, 360);  // ~21000 px, beyond both engines' size limits

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
