// C++/WinRT first, so Qt's keyword macros cannot leak into its headers.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include "ocr.h"

#ifdef JIEYING_HAVE_PADDLEOCR
#include "paddleocr.h"
#endif

#include <QCoreApplication>
#include <QtConcurrent>

#include <cstring>

using namespace winrt::Windows::Graphics::Imaging;
using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Storage::Streams::Buffer;

namespace {

constexpr int kUpscaleBelow = 1500; // small captures recognize far better at 2x

bool isLatin(QChar c)
{
    return c.unicode() < 0x2E80; // everything before the CJK radical/ideograph blocks
}

QString fromHString(const winrt::hstring &s)
{
    return QString::fromWCharArray(s.c_str(), int(s.size()));
}

OcrEngine createEngine()
{
    OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) {
        const auto languages = OcrEngine::AvailableRecognizerLanguages();
        if (languages.Size() > 0)
            engine = OcrEngine::TryCreateFromLanguage(languages.GetAt(0));
    }
    return engine;
}

} // namespace

QString OcrResult::text() const
{
    QStringList texts;
    for (const OcrLine &line : lines)
        texts << line.text;
    return texts.join(QLatin1Char('\n'));
}

QString Ocr::joinWords(const QList<Word> &words)
{
    // A real space is about a quarter of the text height; the recognizer also splits single
    // Latin words ("Wo" "rld") with almost no gap, and those must be glued back.
    constexpr qreal kSpaceGap = 0.2; // of the taller word's height

    QString line;
    QRectF previous;
    for (const Word &word : words) {
        if (word.text.isEmpty())
            continue;
        if (!line.isEmpty()) {
            const bool cjkBoth = !isLatin(line.back()) && !isLatin(word.text.front());
            const qreal gap = word.box.left() - previous.right();
            if (!cjkBoth && gap >= kSpaceGap * qMax(word.box.height(), previous.height()))
                line += QLatin1Char(' ');
        }
        line += word.text;
        previous = word.box;
    }
    return line;
}

namespace {

bool isAsciiAlnum(QChar c)
{
    return c.unicode() < 128 && c.isLetterOrNumber();
}

// A Chinese/Japanese character, not punctuation.
bool isCjkLetter(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF);
}

} // namespace

QString Ocr::fixPunctuation(const QString &line)
{
    static const QString fullWidth = QStringLiteral("\uFF1A\uFF0C\uFF1B\uFF0E"); // ： ， ； ．
    static const QString halfWidth = QStringLiteral(":,;.");
    static const QString toFull = QStringLiteral(":,;?!");
    static const QString toFullTarget = QStringLiteral("\uFF1A\uFF0C\uFF1B\uFF1F\uFF01");

    QString s = line;
    const QChar none;
    auto at = [&s, none](qsizetype i) { return i >= 0 && i < s.size() ? s[i] : none; };
    QChar openBracket; // the kind of the bracket still open, to close it the same way
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s[i], prev = at(i - 1), next = at(i + 1);
        if (const qsizetype k = fullWidth.indexOf(c); k >= 0) {
            // Recognizers often put spaces around full-width marks ("D \uFF1A \\Docs"); look past them.
            qsizetype before = i - 1, after = i + 1;
            while (at(before).isSpace())
                --before;
            while (at(after).isSpace())
                ++after;
            const QChar beyond = at(after);
            const bool asciiAfter = isAsciiAlnum(beyond) || beyond == QLatin1Char('\\') || beyond == QLatin1Char('/');
            if (isAsciiAlnum(at(before)) && asciiAfter) {
                s[i] = halfWidth[k];
                if (c != QChar(0xFF0C) || beyond.isDigit()) // "1, 286" -> "1,286", but "a, b" keeps its space
                    s.remove(i + 1, after - i - 1);
                s.remove(before + 1, i - before - 1); // never a space before the mark
                i = before + 1;
            }
        } else if (const qsizetype k = toFull.indexOf(c); k >= 0) {
            const bool latinAfter = isAsciiAlnum(next) && !next.isDigit();
            if (isCjkLetter(prev) && !latinAfter && next != QLatin1Char('/') && next != QLatin1Char('\\'))
                s[i] = toFullTarget[k];
        } else if (c == QLatin1Char('(') || c == QChar(0xFF08)) {
            openBracket = c;
        } else if (c == QLatin1Char(')') || c == QChar(0xFF09)) {
            if (!openBracket.isNull())
                s[i] = openBracket == QLatin1Char('(') ? QLatin1Char(')') : QChar(0xFF09);
            openBracket = QChar();
        } else if ((c == QChar(0x2014) || c == QChar(0x2013)) && isCjkLetter(prev) && isCjkLetter(next)) {
            s[i] = QChar(0x4E00); // 下—页 -> 下一页
        } else if (c == QChar(0x2014) && (prev.isNull() || prev.isSpace()) && isAsciiAlnum(next)) {
            s.replace(i, 1, QStringLiteral("--")); // --option read as one long dash
            ++i;
        } else if ((c == QLatin1Char('O') || c == QLatin1Char('o')) && prev.isDigit() && (next.isDigit() || isCjkLetter(next))) {
            s[i] = QLatin1Char('0');
        } else if ((c == QLatin1Char('O') || c == QLatin1Char('o')) && next == QLatin1Char('x') && !isAsciiAlnum(prev)
                   && QStringLiteral("0123456789abcdefABCDEF").contains(at(i + 2))) {
            s[i] = QLatin1Char('0'); // Ox1F -> 0x1F
        }
    }
    return s;
}

namespace {

OcrResult recognizeWhole(const QImage &image, Config::OcrEngine engine)
{
#ifdef JIEYING_HAVE_PADDLEOCR
    if (engine != Config::OcrEngine::Windows && PaddleOcr::isAvailable()) {
        try {
            return PaddleOcr::recognize(image);
        } catch (const std::exception &e) {
            qWarning("PaddleOCR failed, using Windows OCR: %s", e.what());
        }
    }
#else
    Q_UNUSED(engine);
#endif
    return Ocr::recognizeWindows(image);
}

} // namespace

namespace {

OcrResult recognizeStrips(const QImage &image, Config::OcrEngine engine);

} // namespace

OcrResult Ocr::recognize(const QImage &image, Config::OcrEngine engine)
{
    OcrResult result = recognizeStrips(image, engine);
    for (OcrLine &line : result.lines)
        line.text = fixPunctuation(line.text);
    return result;
}

namespace {

OcrResult recognizeStrips(const QImage &image, Config::OcrEngine engine)
{
    // Engines shrink big images to a fixed size, which makes the text of a long screenshot
    // unreadable. Tall images are read in overlapping strips instead; a line is kept by the
    // strip whose core (the part outside the shared overlap halves) holds its center.
    // Strips plus overlaps stay within PaddleOCR's 2560 px detection size, so nothing is shrunk.
    constexpr int kOverlap = 120;
    constexpr int strip = 2300;
    if (image.height() <= strip + kOverlap)
        return recognizeWhole(image, engine);

    OcrResult result;
    for (int top = 0; top < image.height(); top += strip) {
        const int from = qMax(0, top - kOverlap);
        const int to = qMin(image.height(), top + strip + kOverlap);
        const OcrResult part = recognizeWhole(image.copy(0, from, image.width(), to - from), engine);
        if (!part.error.isEmpty())
            return part;
        result.language = part.language;
        for (OcrLine line : part.lines) {
            line.box.translate(0, from);
            const int center = line.box.center().y();
            if (center >= top && center < top + strip)
                result.lines.append(line);
        }
    }
    return result;
}

} // namespace

void Ocr::warmUpInBackground()
{
#ifdef JIEYING_HAVE_PADDLEOCR
    if (PaddleOcr::isAvailable())
        (void)QtConcurrent::run(&PaddleOcr::warmUp);
#endif
}

OcrResult Ocr::recognizeWindows(const QImage &source)
{
    OcrResult result;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error &) {
        // The pool thread is already in an apartment; WinRT calls work either way.
    }

    try {
        const OcrEngine engine = createEngine();
        if (!engine) {
            result.error = QCoreApplication::translate("Ocr", "系统没有安装 OCR 语言包（设置 → 时间和语言 → 语言）。");
            return result;
        }
        result.language = QStringLiteral("Windows OCR ") + fromHString(engine.RecognizerLanguage().LanguageTag());

        const int maxDimension = int(OcrEngine::MaxImageDimension());
        const int side = qMax(source.width(), source.height());
        qreal scale = 1.0;
        if (side < kUpscaleBelow && side * 2 <= maxDimension)
            scale = 2.0;
        else if (side > maxDimension)
            scale = qreal(maxDimension) / side;

        const QImage scaled = qFuzzyCompare(scale, 1.0)
                                  ? source
                                  : source.scaled(source.size() * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        // ARGB32 premultiplied is BGRA in memory on little-endian, with rows exactly width*4 bytes.
        const QImage image = scaled.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        const auto bytes = uint32_t(image.sizeInBytes());
        Buffer buffer(bytes);
        buffer.Length(bytes);
        std::memcpy(buffer.data(), image.constBits(), bytes);
        SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, image.width(), image.height(), BitmapAlphaMode::Premultiplied);
        bitmap.CopyFromBuffer(buffer);

        const auto recognized = engine.RecognizeAsync(bitmap).get();
        for (const auto &line : recognized.Lines()) {
            QList<Ocr::Word> words;
            QRectF box;
            for (const auto &word : line.Words()) {
                const auto r = word.BoundingRect();
                const QRectF wordBox(r.X, r.Y, r.Width, r.Height);
                words.append({fromHString(word.Text()), wordBox});
                box |= wordBox;
            }
            const QRectF sourceBox(box.x() / scale, box.y() / scale, box.width() / scale, box.height() / scale);
            result.lines.append({joinWords(words), sourceBox.toAlignedRect()});
        }
    } catch (const winrt::hresult_error &e) {
        result.error = fromHString(e.message());
    }
    return result;
}
