#pragma once

#include "ocr.h"

#include <QList>
#include <QRect>
#include <QString>

// Finds personal data in recognized text so it can be covered with one click: phone numbers,
// e-mail addresses, Chinese ID numbers, bank card numbers and IPv4 addresses.
namespace Sensitive {

enum class Kind { Phone, Email, IdCard, BankCard, Ip };

struct Match
{
    int start;
    int length;
    Kind kind;
};

// Matches in text, left to right, overlaps merged. ID numbers must pass their check digit and
// card numbers the Luhn check, so ordinary long numbers are left alone.
QList<Match> find(const QString &text);

// Where text.mid(start, length) sits inside the line's box. Recognizers only report whole
// lines, so the span is estimated from character widths (CJK about twice a digit's width)
// and padded so the estimate errs on the covering side.
QRect locate(const OcrLine &line, int start, int length);

// Boxes to cover in an image whose recognized lines are given.
QList<QRect> regions(const QList<OcrLine> &lines);

} // namespace Sensitive
