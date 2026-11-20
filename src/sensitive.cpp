#include "sensitive.h"

#include <QRegularExpression>

#include <algorithm>

namespace {

QString digitsOf(const QString &s)
{
    QString digits;
    for (const QChar c : s) {
        if (c.isDigit() || c == QLatin1Char('X') || c == QLatin1Char('x'))
            digits += c.toUpper();
    }
    return digits;
}

// GB 11643: weighted sum of the first 17 digits selects the check character.
bool validIdNumber(const QString &id)
{
    static constexpr int kWeights[] = {7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2};
    static constexpr char kCheck[] = "10X98765432";
    if (id.size() != 18)
        return false;
    int sum = 0;
    for (int i = 0; i < 17; ++i) {
        if (!id[i].isDigit())
            return false;
        sum += id[i].digitValue() * kWeights[i];
    }
    return id[17] == QLatin1Char(kCheck[sum % 11]);
}

bool luhnValid(const QString &digits)
{
    int sum = 0;
    bool doubled = false;
    for (qsizetype i = digits.size() - 1; i >= 0; --i) {
        if (!digits[i].isDigit())
            return false;
        int d = digits[i].digitValue();
        if (doubled && (d *= 2) > 9)
            d -= 9;
        sum += d;
        doubled = !doubled;
    }
    return sum % 10 == 0;
}

struct Rule
{
    Sensitive::Kind kind;
    QRegularExpression pattern;
    bool (*accept)(const QString &match);
};

const QList<Rule> &rules()
{
    static const QList<Rule> list = {
        {Sensitive::Kind::Email, QRegularExpression(QStringLiteral(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,})")),
         nullptr},
        // ID before bank card: an 18-digit ID would otherwise only be caught if it passed Luhn.
        {Sensitive::Kind::IdCard, QRegularExpression(QStringLiteral(R"((?<![0-9A-Za-z])\d{17}[0-9Xx](?![0-9A-Za-z]))")),
         [](const QString &m) { return validIdNumber(digitsOf(m)); }},
        // Mainland mobile numbers, optionally +86 and grouped 3-4-4 with spaces or dashes.
        {Sensitive::Kind::Phone, QRegularExpression(QStringLiteral(R"((?<!\d)(?:\+?86[ -]?)?1[3-9]\d(?:[ -]?\d{4}){2}(?!\d))")),
         nullptr},
        // Landlines with an area code: 010-12345678, 0755-1234567.
        {Sensitive::Kind::Phone, QRegularExpression(QStringLiteral(R"((?<!\d)0\d{2,3}-\d{7,8}(?!\d))")), nullptr},
        // 13-19 digit card numbers, optionally in groups of four.
        {Sensitive::Kind::BankCard, QRegularExpression(QStringLiteral(R"((?<!\d)\d{4}(?:[ -]?\d{4}){2,3}(?:[ -]?\d{1,3})?(?!\d))")),
         [](const QString &m) {
             const QString digits = digitsOf(m);
             return digits.size() >= 13 && digits.size() <= 19 && luhnValid(digits);
         }},
        {Sensitive::Kind::Ip, QRegularExpression(QStringLiteral(
                                  R"((?<![\d.])(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?![\d.]))")),
         nullptr},
    };
    return list;
}

// Rough advance of a character in units of the line height.
qreal advance(QChar c)
{
    if (c.unicode() >= 0x2E80 || (c.unicode() >= 0xFF00 && c.unicode() <= 0xFFEF))
        return 1.0; // CJK and full-width forms
    if (c.isSpace())
        return 0.3;
    if (c == QLatin1Char('m') || c == QLatin1Char('w') || c == QLatin1Char('M') || c == QLatin1Char('W')
        || c == QLatin1Char('@'))
        return 0.8;
    return 0.55;
}

} // namespace

namespace Sensitive {

QList<Match> find(const QString &text)
{
    QList<Match> found;
    for (const Rule &rule : rules()) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (!rule.accept || rule.accept(m.captured()))
                found.append({int(m.capturedStart()), int(m.capturedLength()), rule.kind});
        }
    }
    std::sort(found.begin(), found.end(), [](const Match &a, const Match &b) { return a.start < b.start; });

    QList<Match> merged;
    for (const Match &m : std::as_const(found)) {
        if (!merged.isEmpty() && m.start < merged.last().start + merged.last().length) {
            Match &last = merged.last();
            last.length = qMax(last.start + last.length, m.start + m.length) - last.start;
        } else {
            merged.append(m);
        }
    }
    return merged;
}

QRect locate(const OcrLine &line, int start, int length)
{
    qreal before = 0, span = 0, total = 0;
    for (int i = 0; i < line.text.size(); ++i) {
        const qreal a = advance(line.text[i]);
        if (i < start)
            before += a;
        else if (i < start + length)
            span += a;
        total += a;
    }
    if (total <= 0)
        return line.box;
    const QRectF box(line.box);
    const qreal scale = box.width() / total;
    const qreal padX = box.height() * 0.3;
    const qreal padY = box.height() * 0.15;
    QRectF cover(box.left() + before * scale - padX, box.top() - padY, span * scale + 2 * padX,
                 box.height() + 2 * padY);
    cover &= box.adjusted(-padX, -padY, padX, padY);
    return cover.toAlignedRect();
}

QList<QRect> regions(const QList<OcrLine> &lines)
{
    QList<QRect> boxes;
    for (const OcrLine &line : lines) {
        for (const Match &m : find(line.text))
            boxes.append(locate(line, m.start, m.length));
    }
    return boxes;
}

} // namespace Sensitive
