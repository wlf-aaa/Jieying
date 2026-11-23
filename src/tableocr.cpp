#include "tableocr.h"

#include "ocr.h"

#include <QCoreApplication>
#include <QPainter>
#include <QRegularExpression>

#include <algorithm>

namespace {

constexpr int kMaxDimension = 12000;
constexpr qint64 kMaxPixels = 24000000;
constexpr int kMaxCells = 1000;
constexpr int kMaxRules = 1600;
constexpr int kBorderTolerance = 9;
constexpr int kCellInset = 3;
constexpr int kBatchSize = 1440;
constexpr int kBatchPadding = 18;

struct Rule
{
    int position;
    int first;
    int last;
};

struct Grid
{
    QList<int> x;
    QList<int> y;
};

bool near(int a, int b)
{
    return qAbs(a - b) <= kBorderTolerance;
}

// Find thin high-contrast strokes in either direction. Intersections can interrupt a
// stroke for a few pixels, because the neighboring samples are part of the crossing rule.
QList<Rule> scanRules(const QImage &gray, Qt::Orientation orientation)
{
    constexpr int kProbe = 3;
    constexpr int kContrast = 12;
    constexpr int kMaxGap = 7;
    const bool horizontal = orientation == Qt::Horizontal;
    const int length = horizontal ? gray.width() : gray.height();
    const int count = horizontal ? gray.height() : gray.width();
    const int minLength = horizontal ? 32 : 20;
    const qsizetype positionStep = horizontal ? gray.bytesPerLine() : 1;
    const qsizetype lengthStep = horizontal ? 1 : gray.bytesPerLine();
    QList<Rule> rules;
    for (int position = 0; position < count; ++position) {
        const int before = position >= kProbe ? position - kProbe : position + kProbe;
        const int after = position + kProbe < count ? position + kProbe : before;
        const uchar *line = gray.constBits() + position * positionStep;
        const uchar *prev = gray.constBits() + before * positionStep;
        const uchar *next = gray.constBits() + after * positionStep;
        int first = -1;
        int last = -1;
        int hits = 0;
        for (int i = 0; i <= length; ++i) {
            if (i < length) {
                const qsizetype offset = i * lengthStep;
                const int a = line[offset] - prev[offset];
                const int b = line[offset] - next[offset];
                if ((a >= kContrast && b >= kContrast) || (a <= -kContrast && b <= -kContrast)) {
                    if (first < 0)
                        first = i;
                    last = i;
                    ++hits;
                    continue;
                }
                if (first < 0 || i - last <= kMaxGap)
                    continue;
            }
            const int span = last - first + 1;
            if (first >= 0 && hits * 4 >= span * 3 && span >= minLength) {
                // Several adjacent scan lines can describe one thick border.
                auto existing = rules.rbegin();
                while (existing != rules.rend() && position - existing->position <= kProbe) {
                    if (qAbs(existing->first - first) <= kProbe && qAbs(existing->last - last) <= kProbe)
                        break;
                    ++existing;
                }
                if (existing != rules.rend() && position - existing->position <= kProbe) {
                    existing->position = position;
                } else {
                    rules.append({position, first, last});
                    if (rules.size() > kMaxRules)
                        return rules;
                }
            }
            first = -1;
            hits = 0;
        }
    }
    std::sort(rules.begin(), rules.end(), [](const Rule &a, const Rule &b) { return a.position < b.position; });
    return rules;
}

template <typename Predicate>
const Rule *findRule(const QList<Rule> &rules, int position, Predicate matches)
{
    const auto first = std::lower_bound(rules.cbegin(), rules.cend(), position - kBorderTolerance,
                                        [](const Rule &rule, int edge) { return rule.position < edge; });
    for (auto it = first; it != rules.cend() && it->position <= position + kBorderTolerance; ++it) {
        if (matches(*it))
            return &*it;
    }
    return nullptr;
}

// Each candidate starts at an observed top edge. All four outer sides must meet at the corners.
QRect tableBounds(const QList<Rule> &horizontal, const QList<Rule> &vertical)
{
    QRect best;
    for (const Rule &top : horizontal) {
        const auto *left = findRule(vertical, top.first,
                                    [&](const Rule &rule) { return near(rule.first, top.position); });
        if (!left)
            continue;
        const auto *right = findRule(vertical, top.last, [&](const Rule &rule) {
            return near(rule.first, top.position) && near(rule.last, left->last);
        });
        if (!right)
            continue;
        const auto *bottom = findRule(horizontal, left->last, [&](const Rule &rule) {
            return near(rule.first, left->position) && near(rule.last, right->position);
        });
        if (!bottom)
            continue;
        const QRect area(QPoint(left->position, top.position), QPoint(right->position, bottom->position));
        if (area.isValid() && qint64(area.width()) * area.height() > qint64(best.width()) * best.height())
            best = area;
    }
    return best;
}

QList<int> fullRules(const QList<Rule> &rules, int from, int to, int low, int high)
{
    QList<int> positions;
    for (const Rule &rule : rules) {
        if (rule.position < from || rule.position > to || !near(rule.first, low) || !near(rule.last, high))
            continue;
        if (positions.isEmpty() || rule.position - positions.last() > kBorderTolerance)
            positions.append(rule.position);
    }
    return positions;
}

// A partial divider that joins two perpendicular rules indicates a merged or uneven cell.
bool hasPartialRules(const QList<Rule> &rules, const QList<int> &positions, const QList<int> &crossings)
{
    const auto hitsBoundary = [](const QList<int> &boundaries, int edge) {
        return std::any_of(boundaries.cbegin(), boundaries.cend(), [edge](int value) { return near(value, edge); });
    };
    for (const Rule &rule : rules) {
        if (rule.position <= positions.first() || rule.position >= positions.last())
            continue;
        if (near(rule.first, crossings.first()) && near(rule.last, crossings.last()))
            continue;
        if (hitsBoundary(crossings, rule.first) && hitsBoundary(crossings, rule.last))
            return true;
    }
    return false;
}

Grid findGrid(const QList<Rule> &horizontal, const QList<Rule> &vertical)
{
    const QRect area = tableBounds(horizontal, vertical);
    if (!area.isValid())
        return {};
    Grid grid{
        fullRules(vertical, area.left(), area.right(), area.top(), area.bottom()),
        fullRules(horizontal, area.top(), area.bottom(), area.left(), area.right()),
    };
    if (grid.x.size() < 2 || grid.y.size() < 2 || hasPartialRules(vertical, grid.x, grid.y)
        || hasPartialRules(horizontal, grid.y, grid.x))
        return {};
    return grid;
}

bool hasInk(const QImage &gray, const QRect &rect)
{
    const int background = gray.constScanLine(rect.top())[rect.left()];
    int different = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *line = gray.constScanLine(y);
        for (int x = rect.left(); x <= rect.right(); ++x) {
            if (qAbs(line[x] - background) >= 14 && ++different >= 4)
                return true;
        }
    }
    return false;
}

struct Crop
{
    int row;
    int column;
    QRect source;
    QRect target;
};

QString readBatch(const QImage &image, const QList<Crop> &crops, Config::OcrEngine engine,
                  QList<QStringList> &cells)
{
    if (crops.isEmpty())
        return {};
    int width = 0;
    for (const Crop &crop : crops)
        width = qMax(width, crop.target.width());
    QImage batch(width + 2 * kBatchPadding, crops.last().target.bottom() + kBatchPadding + 1, QImage::Format_RGB32);
    if (batch.isNull())
        return QCoreApplication::translate("TableOcr", "无法分配表格识别图像。");
    batch.fill(Qt::white);
    {
        QPainter painter(&batch);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        for (const Crop &crop : crops)
            painter.drawImage(crop.target, image, crop.source);
    }

    // Each band contains just one cell, so neither engine can join adjacent columns as one line.
    const OcrResult recognized = Ocr::recognize(batch, engine);
    for (const OcrLine &line : recognized.lines) {
        const QPoint center = line.box.center();
        const auto crop = std::find_if(crops.cbegin(), crops.cend(),
                                       [&](const Crop &c) { return c.target.contains(center); });
        if (crop == crops.cend() || line.text.trimmed().isEmpty())
            continue;
        QString &cell = cells[crop->row][crop->column];
        if (!cell.isEmpty())
            cell += QLatin1Char('\n');
        cell += line.text.trimmed();
    }
    return recognized.error;
}

TableOcr::Result recognizeGrid(const QImage &image, const QImage &gray, const Grid &grid, Config::OcrEngine engine)
{
    const int rows = int(grid.y.size()) - 1;
    const int columns = int(grid.x.size()) - 1;
    if (rows * columns > kMaxCells)
        return {{}, QCoreApplication::translate("TableOcr", "表格单元格超过 %1 个，请分段截图。").arg(kMaxCells), false};

    TableOcr::Result result;
    result.hasGrid = true;
    result.cells = QList<QStringList>(rows, QStringList(columns));
    QList<Crop> batch;
    int nextY = kBatchPadding;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const QRect source(QPoint(grid.x[column] + kCellInset, grid.y[row] + kCellInset),
                               QPoint(grid.x[column + 1] - kCellInset, grid.y[row + 1] - kCellInset));
            if (!source.isValid() || !hasInk(gray, source))
                continue;
            QSize size = source.size();
            constexpr int maxSide = kBatchSize - 2 * kBatchPadding;
            if (size.width() > maxSide || size.height() > maxSide)
                size = size.scaled(maxSide, maxSide, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
            if (nextY + size.height() + kBatchPadding > kBatchSize) {
                const auto error = readBatch(image, batch, engine, result.cells);
                if (!error.isEmpty())
                    return {{}, error, false};
                batch.clear();
                nextY = kBatchPadding;
            }
            batch.append({row, column, source, QRect(QPoint(kBatchPadding, nextY), size)});
            nextY += size.height() + 2 * kBatchPadding;
        }
    }
    const auto error = readBatch(image, batch, engine, result.cells);
    if (!error.isEmpty())
        return {{}, error, false};
    return result;
}

} // namespace

TableOcr::Result TableOcr::recognize(const QImage &image, Config::OcrEngine engine)
{
    if (image.isNull())
        return {{}, QCoreApplication::translate("TableOcr", "没有可识别的图像。"), false};
    if (qMax(image.width(), image.height()) > kMaxDimension || qint64(image.width()) * image.height() > kMaxPixels)
        return {{}, QCoreApplication::translate("TableOcr", "图片过大，请分段截图识别表格。"), false};

    if (image.width() >= 32 && image.height() >= 20) {
        const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
        if (gray.isNull())
            return {{}, QCoreApplication::translate("TableOcr", "无法分配表格识别图像。"), false};
        const auto horizontal = scanRules(gray, Qt::Horizontal);
        const auto vertical = scanRules(gray, Qt::Vertical);
        if (horizontal.size() > kMaxRules || vertical.size() > kMaxRules)
            return {{}, QCoreApplication::translate("TableOcr", "图片线条过多，请截取较小的表格区域。"), false};
        const Grid grid = findGrid(horizontal, vertical);
        if (!grid.x.isEmpty())
            return recognizeGrid(image, gray, grid, engine);
    }

    const OcrResult text = Ocr::recognize(image, engine);
    if (!text.error.isEmpty())
        return {{}, text.error, false};
    TableOcr::Result result;
    for (const auto &line : text.lines) {
        if (line.text.trimmed().isEmpty())
            continue;
        if (result.cells.size() == kMaxCells)
            return {{}, QCoreApplication::translate("TableOcr", "文字行超过 %1 行，请分段截图。").arg(kMaxCells), false};
        result.cells.append(QStringList{line.text});
    }
    return result;
}

QString TableOcr::toTsv(const QList<QStringList> &cells)
{
    static const QRegularExpression embeddedSeparators(QStringLiteral("[\\t\\r\\n\\x{2028}\\x{2029}]+"));
    QStringList rows;
    rows.reserve(cells.size());
    for (QStringList row : cells) {
        for (QString &cell : row)
            cell.replace(embeddedSeparators, QStringLiteral(" "));
        rows.append(row.join(QLatin1Char('\t')));
    }
    return rows.join(QLatin1Char('\n'));
}
