#include "selfcheck.h"

#include "barcodes.h"
#include "frameencoder.h"
#include "ocr.h"
#ifdef JIEYING_HAVE_PADDLEOCR
#include "paddleocr.h"
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSettings>
#include <QSslSocket>
#include <QStandardPaths>
#include <QtConcurrent>

namespace {

const QString kSample = QStringLiteral("截影自检 Self Check 2026");

QString compact(QString text)
{
    text.remove(QLatin1Char(' '));
    return text;
}

QImage sampleImage()
{
    QImage image(520, 80, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(26);
    p.setFont(font);
    p.setPen(Qt::black);
    p.drawText(QRect(20, 0, 500, 80), Qt::AlignVCenter, kSample);
    return image;
}

bool encodes(FrameEncoder &encoder, const QString &path)
{
    QImage frame(160, 120, QImage::Format_RGB32);
    bool ok = encoder.open(path, frame.size(), 10);
    for (int i = 0; ok && i < 3; ++i) {
        frame.fill(QColor::fromHsv(i * 100, 200, 220));
        ok = encoder.addFrame(frame, i * 100);
    }
    ok = ok && encoder.finish(300) && QFileInfo(path).size() > 0;
    QFile::remove(path);
    return ok;
}

} // namespace

int runSelfCheck(const QString &reportPath)
{
    QStringList report;
    int failures = 0;
    auto check = [&](bool ok, const QString &what, const QString &detail = QString()) {
        report << QStringLiteral("%1  %2%3").arg(ok ? "PASS" : "FAIL", what, detail.isEmpty() ? QString() : "  — " + detail);
        failures += ok ? 0 : 1;
    };
    auto info = [&](const QString &what) { report << QStringLiteral("INFO  ") + what; };

    info(QStringLiteral("截影 %1, %2").arg(QCoreApplication::applicationVersion(),
                                           QDateTime::currentDateTime().toString(Qt::ISODate)));
    info(QStringLiteral("exe: ") + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    info(QStringLiteral("settings: ") + QDir::toNativeSeparators(QSettings().fileName()));

    check(QCoreApplication::translate("QPlatformTheme", "Cancel") != QLatin1String("Cancel"),
          QStringLiteral("Qt Chinese translation loaded"));
    const auto writers = QImageWriter::supportedImageFormats();
    const auto readers = QImageReader::supportedImageFormats();
    check(writers.contains("png") && writers.contains("jpg") && writers.contains("webp") && readers.contains("gif")
              && readers.contains("ico"),
          QStringLiteral("image format plugins (png, jpg, webp, gif, ico)"));
    check(QSslSocket::supportsSsl(), QStringLiteral("HTTPS for translation (TLS backend)"),
          QSslSocket::activeBackend());
    const QImage icon = QIcon(QStringLiteral(":/icons/rect.svg")).pixmap(QSize(24, 24)).toImage();
    bool ink = false;
    for (int y = 0; y < icon.height() && !ink; ++y)
        for (int x = 0; x < icon.width() && !ink; ++x)
            ink = qAlpha(icon.pixel(x, y)) > 0;
    check(ink, QStringLiteral("toolbar icons render (SVG icon engine)"));

    // OCR: Windows.Media.Ocr is required; PaddleOCR is optional (the default engine uses it when present).
    const QImage sample = sampleImage();
    auto recognize = [&sample](OcrResult (*engine)(const QImage &)) {
        return QtConcurrent::run([&sample, engine] { return engine(sample); }).result();
    };
    const OcrResult windows = recognize(&Ocr::recognizeWindows);
    check(windows.error.isEmpty() && !windows.lines.isEmpty(), QStringLiteral("Windows OCR runs"),
          windows.error.isEmpty() ? windows.text() : windows.error);
#ifdef JIEYING_HAVE_PADDLEOCR
    if (PaddleOcr::isAvailable()) {
        const OcrResult paddle = QtConcurrent::run([&sample] {
                                     try {
                                         return PaddleOcr::recognize(sample);
                                     } catch (const std::exception &e) {
                                         OcrResult failed;
                                         failed.error = QString::fromLocal8Bit(e.what());
                                         return failed;
                                     }
                                 }).result();
        check(paddle.error.isEmpty() && compact(paddle.text()) == compact(kSample), QStringLiteral("PaddleOCR reads the sample"),
              paddle.error.isEmpty() ? paddle.text() : paddle.error);
    } else {
        info(QStringLiteral("PaddleOCR not installed (onnxruntime.dll / ocr-models missing): Windows OCR is used"));
    }
#endif
    const OcrResult byDefault = QtConcurrent::run([&sample] { return Ocr::recognize(sample); }).result();
    check(compact(byDefault.text()).contains(QStringLiteral("截影自检")),
          QStringLiteral("default OCR engine reads the sample"), byDefault.language + ": " + byDefault.text());

#ifdef JIEYING_HAVE_ZXING
    const QString qrText = QStringLiteral("截影 self-check QR");
    const QList<Barcode> codes = Barcodes::detect(Barcodes::makeQrCode(qrText, 4));
    check(codes.size() == 1 && codes.first().text == qrText, QStringLiteral("QR code reading (ZXing-cpp)"));
#endif

    const QDir temp(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    GifEncoder gif;
    check(encodes(gif, temp.filePath("jieying_selfcheck.gif")), QStringLiteral("GIF encoder"), gif.errorString());
    Mp4Encoder mp4;
    mp4.setAudio(true);
    check(encodes(mp4, temp.filePath("jieying_selfcheck.mp4")), QStringLiteral("MP4 encoder (Media Foundation H.264)"),
          mp4.errorString());
    check(mp4.hasAudio(), QStringLiteral("MP4 sound track (AAC encoder)"));

    report << (failures ? QStringLiteral("RESULT  FAILED (%1)").arg(failures) : QStringLiteral("RESULT  OK"));
    QFile file(reportPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        file.write(report.join(QLatin1Char('\n')).toUtf8() + '\n');
    return failures ? 1 : 0;
}
