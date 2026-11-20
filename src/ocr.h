#pragma once

#include "config.h"

#include <QImage>
#include <QList>
#include <QRect>
#include <QString>

struct OcrLine
{
    QString text;
    QRect box; // in source image pixels
};

struct OcrResult
{
    QList<OcrLine> lines;
    QString language; // BCP-47 tag of the recognizer used
    QString error;    // non-empty when recognition failed

    QString text() const;
};

namespace Ocr {

// Blocking; run it on a worker thread, never the GUI thread. Auto uses PaddleOCR when its runtime
// and models are installed; PaddleOCR falls back to Windows.Media.Ocr when unavailable or failing.
OcrResult recognize(const QImage &image, Config::OcrEngine engine = Config::OcrEngine::Auto);

// The Windows.Media.Ocr engine alone.
OcrResult recognizeWindows(const QImage &image);

// Starts loading the default engine in the background, so a later recognize() is quick.
void warmUpInBackground();

struct Word
{
    QString text;
    QRectF box;
};

// Joins recognizer words into a line. Windows OCR returns CJK text one character per word
// and sometimes splits Latin words, so words are spaced by their actual gap: CJK neighbours
// are always glued, anything else gets a space only when visibly apart.
QString joinWords(const QList<Word> &words);

// Fixes the punctuation recognizers mix up, judged by the neighbours: full-width colons and
// commas between digits or Latin text become half-width ("23\uFF1A05" -> "23:05"), half-width
// ones after Chinese become full-width, brackets close the way they opened, a lone dash between
// two Chinese characters is the character "one", "Ox1F" is "0x1F" and an O after a digit is 0.
QString fixPunctuation(const QString &line);

} // namespace Ocr
