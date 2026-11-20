#include "scrollstitcher.h"

#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kMinEvidenceRows = 8;     // informative rows an offset must explain
constexpr double kMinMatchRatio = 0.9;  // tolerates small animations and blinking carets
constexpr int kCoarseBins = 48;         // brightness samples per row for the quick search
constexpr int kMaxFineBins = 480;       // ... and for the close comparison of the best candidates
constexpr int kCandidates = 8;          // quick-search offsets compared closely
constexpr int kMinRowContrast = 24;     // a row with less brightness range carries no evidence
constexpr int kMaxSampleRows = 240;     // rows compared per candidate offset
constexpr double kKeptShare = 0.8;      // the best-fitting share of rows that decides (hover, animation)
constexpr double kMaxRatioToRunnerUp = 0.7; // the fit must beat any other position this clearly

// Scrollbars change with every scroll; leave the right edge out of the row hashes.
int hashedWidth(int width)
{
    return width > 200 ? width - qMin(40, width / 10) : width;
}

int fineBinsFor(int width)
{
    return qBound(kCoarseBins, hashedWidth(width) / 2, kMaxFineBins);
}

// Average brightness of `bins` equal spans of each row.
std::vector<float> profile(const QImage &frame, int bins)
{
    const int width = hashedWidth(frame.width());
    std::vector<float> values(size_t(frame.height()) * bins);
    for (int y = 0; y < frame.height(); ++y) {
        const auto *pixels = reinterpret_cast<const quint32 *>(frame.constScanLine(y));
        float *row = values.data() + size_t(y) * bins;
        for (int b = 0; b < bins; ++b) {
            const int from = b * width / bins;
            const int to = qMax(from + 1, (b + 1) * width / bins);
            int sum = 0;
            for (int x = from; x < to; ++x)
                sum += qGray(pixels[x]);
            row[b] = float(sum) / float(to - from);
        }
    }
    return values;
}

} // namespace

ScrollStitcher::ScrollStitcher(int maxHeight)
    : m_maxHeight(maxHeight)
{
}

QList<ScrollStitcher::Row> ScrollStitcher::rowsOf(const QImage &frame) const
{
    const int width = hashedWidth(frame.width());
    QList<Row> rows(frame.height());
    for (int y = 0; y < frame.height(); ++y) {
        const auto *pixels = reinterpret_cast<const quint32 *>(frame.constScanLine(y));
        quint64 hash = 14695981039346656037ull; // FNV-1a
        bool uniform = true;
        int low = 255, high = 0;
        for (int x = 0; x < width; ++x) {
            hash = (hash ^ pixels[x]) * 1099511628211ull;
            uniform = uniform && pixels[x] == pixels[0];
            const int gray = qGray(pixels[x]);
            low = qMin(low, gray);
            high = qMax(high, gray);
        }
        rows[y] = {hash, uniform, high - low >= kMinRowContrast};
    }
    return rows;
}

ScrollStitcher::Profiles ScrollStitcher::profilesOf(const QImage &frame) const
{
    return {profile(frame, kCoarseBins), profile(frame, fineBinsFor(frame.width()))};
}

int ScrollStitcher::findOffsetApprox(const QList<Row> &rows, const Profiles &profiles, int bandStart, int bandEnd) const
{
    // Rows with detail, spread over the band.
    std::vector<int> samples;
    for (int y = bandStart; y < bandEnd; ++y) {
        if (rows[y].informative)
            samples.push_back(y);
    }
    if (int(samples.size()) < kMinEvidenceRows)
        return -1;
    if (int(samples.size()) > kMaxSampleRows) {
        std::vector<int> spread;
        for (int i = 0; i < kMaxSampleRows; ++i)
            spread.push_back(samples[size_t(i) * samples.size() / kMaxSampleRows]);
        samples.swap(spread);
    }

    // How well new row y matches the previous frame at y + offset + t (t: a fraction of a pixel,
    // as a linear blend of two rows): mean difference of the best-fitting share of rows.
    std::vector<float> errors;
    auto fit = [&](const std::vector<float> &now, const std::vector<float> &before, int bins, int offset,
                   float t) -> double {
        errors.clear();
        for (int y : samples) {
            const int at = y + offset;
            if (at + (t == 0.f ? 0 : 1) >= bandEnd)
                break;
            const float *n = now.data() + size_t(y) * bins;
            const float *a = before.data() + size_t(at) * bins;
            const float *b = t == 0.f ? a : a + bins;
            float sum = 0;
            for (int i = 0; i < bins; ++i)
                sum += std::fabs(n[i] - (a[i] + (b[i] - a[i]) * t));
            errors.push_back(sum / float(bins));
        }
        if (int(errors.size()) < kMinEvidenceRows)
            return -1;
        const size_t kept = qMax<size_t>(kMinEvidenceRows, size_t(double(errors.size()) * kKeptShare));
        std::nth_element(errors.begin(), errors.begin() + std::ptrdiff_t(kept - 1), errors.end());
        double total = 0;
        for (size_t i = 0; i < kept; ++i)
            total += errors[i];
        return total / double(kept);
    };

    // Quick search over every offset with coarse profiles.
    std::vector<std::pair<double, int>> coarse; // (error, offset)
    for (int offset = 0; offset <= bandEnd - bandStart - kMinEvidenceRows; ++offset) {
        const double error = fit(profiles.coarse, m_previousProfiles.coarse, kCoarseBins, offset, 0.f);
        if (error >= 0)
            coarse.emplace_back(error, offset);
    }
    if (coarse.size() < 2)
        return -1;
    std::sort(coarse.begin(), coarse.end());

    // Close comparison of the best candidates, to a quarter pixel, with fine profiles: text that
    // differs in a few characters only (lines of a list) looks alike in coarse profiles.
    const int bins = fineBinsFor(m_previous.width());
    struct Fit
    {
        double error;
        double shift;
    };
    std::vector<Fit> fits;
    for (size_t i = 0; i < qMin<size_t>(kCandidates, coarse.size()); ++i) {
        const int base = coarse[i].second;
        Fit best{fit(profiles.fine, m_previousProfiles.fine, bins, base, 0.f), double(base)};
        for (int from : {base - 1, base}) {
            if (from < 0)
                continue;
            for (float t : {0.25f, 0.5f, 0.75f}) {
                const double error = fit(profiles.fine, m_previousProfiles.fine, bins, from, t);
                if (error >= 0 && (best.error < 0 || error < best.error))
                    best = {error, from + t};
            }
        }
        if (best.error >= 0)
            fits.push_back(best);
    }
    if (fits.empty())
        return -1;
    std::sort(fits.begin(), fits.end(), [](const Fit &a, const Fit &b) { return a.error < b.error; });
    const Fit &best = fits.front();
    double runnerUp = 1e9; // the best fit somewhere else: repeating content fits there too
    for (const Fit &other : fits) {
        if (std::abs(other.shift - best.shift) > 3) {
            runnerUp = other.error;
            break;
        }
    }
    if (best.error > kMaxRatioToRunnerUp * runnerUp && runnerUp < 1e9)
        return -1;
    return int(std::lround(best.shift));
}

int ScrollStitcher::findOffset(const QList<Row> &rows, int bandStart, int bandEnd) const
{
    // New row y shows what the previous frame had at row y + offset.
    int bestOffset = 0;
    int bestMatches = 0;
    for (int offset = 1; offset <= bandEnd - bandStart - kMinEvidenceRows; ++offset) {
        int evidence = 0;
        int matches = 0;
        int misses = 0;
        const int budget = int((bandEnd - bandStart - offset) * (1 - kMinMatchRatio)) + 1;
        for (int y = bandStart; y < bandEnd - offset && misses <= budget; ++y) {
            const Row &before = m_previousRows[y + offset];
            const Row &now = rows[y];
            if (before.uniform && now.uniform && before.hash == now.hash)
                continue;
            ++evidence;
            if (before.hash == now.hash)
                ++matches;
            else
                ++misses;
        }
        if (evidence >= kMinEvidenceRows && matches >= kMinMatchRatio * evidence && matches > bestMatches) {
            bestMatches = matches;
            bestOffset = offset;
        }
    }
    return bestOffset;
}

ScrollStitcher::Status ScrollStitcher::add(const QImage &input)
{
    const QImage frame = input.convertToFormat(QImage::Format_RGB32);
    Profiles profiles = profilesOf(frame);
    if (m_previous.isNull()) {
        m_canvas = QImage(frame.width(), qMin(frame.height() * 4, m_maxHeight), QImage::Format_RGB32);
        m_height = 0;
        appendRows(frame, 0, frame.height());
        m_previous = frame;
        m_previousRows = rowsOf(frame);
        m_previousProfiles = std::move(profiles);
        return Status::Started;
    }
    if (frame.size() != m_previous.size())
        return Status::Lost;
    if (m_height >= m_maxHeight)
        return Status::Full;

    const QList<Row> rows = rowsOf(frame);
    const int h = frame.height();
    int top = 0;
    while (top < h && rows[top].hash == m_previousRows[top].hash)
        ++top;
    if (top == h)
        return Status::Unchanged;
    int bottom = 0;
    while (bottom < h - top && rows[h - 1 - bottom].hash == m_previousRows[h - 1 - bottom].hash)
        ++bottom;

    const int bandEnd = h - bottom;
    int offset = findOffset(rows, top, bandEnd);
    if (offset == 0) {
        offset = findOffsetApprox(rows, profiles, top, bandEnd);
        if (offset < 0)
            return Status::Lost;
        if (offset == 0)
            return Status::Unchanged; // only small changes in place (a caret, an animation)
    }

    // The canvas ends with the previous frame's bottom rows: drop its static footer, then add
    // the newly revealed rows followed by the footer again.
    const int added = qMin(offset, m_maxHeight - m_height + bottom);
    m_height -= bottom;
    appendRows(frame, bandEnd - offset, added + bottom);
    m_previous = frame;
    m_previousRows = rows;
    m_previousProfiles = std::move(profiles);
    return added < offset ? Status::Full : Status::Appended;
}

void ScrollStitcher::appendRows(const QImage &frame, int from, int count)
{
    if (m_height + count > m_canvas.height()) {
        const int capacity = qMin(m_maxHeight + frame.height(), qMax(m_height + count, m_canvas.height() * 2));
        QImage grown(m_canvas.width(), capacity, QImage::Format_RGB32);
        for (int y = 0; y < m_height; ++y)
            std::memcpy(grown.scanLine(y), m_canvas.constScanLine(y), size_t(m_canvas.bytesPerLine()));
        m_canvas = grown;
    }
    for (int i = 0; i < count; ++i)
        std::memcpy(m_canvas.scanLine(m_height + i), frame.constScanLine(from + i), size_t(frame.bytesPerLine()));
    m_height += count;
}

QImage ScrollStitcher::result() const
{
    return m_canvas.copy(0, 0, m_canvas.width(), m_height);
}

QImage ScrollStitcher::tail(int rows) const
{
    const int count = qMin(rows, m_height);
    return m_canvas.copy(0, m_height - count, m_canvas.width(), count);
}

// ---- TwoWayStitcher ---------------------------------------------------------------------------

namespace {

// Mirrored along the diagonal: columns become rows, so content scrolling right arrives at the
// bottom, the horizontal scrollbar lands on the right edge (left out of matching like a vertical
// one) and a fixed left sidebar becomes a sticky header. Its own inverse.
QImage transposed(const QImage &image)
{
    return image.transformed(QTransform(0, 1, 1, 0, 0, 0));
}

} // namespace

TwoWayStitcher::TwoWayStitcher(int maxHeight, int maxWidth)
    : m_down(maxHeight)
    , m_right(maxWidth)
{
}

ScrollStitcher::Status TwoWayStitcher::add(const QImage &frame)
{
    using Status = ScrollStitcher::Status;
    switch (m_direction) {
    case Direction::Down:
        return m_down.add(frame);
    case Direction::Right:
        return m_right.add(transposed(frame));
    case Direction::Unknown:
        break;
    }
    const Status down = m_down.add(frame);
    const Status right = m_right.add(transposed(frame));
    if (down == Status::Appended || down == Status::Full) { // vertical first: by far the common case
        m_direction = Direction::Down;
        return down;
    }
    if (right == Status::Appended || right == Status::Full) {
        m_direction = Direction::Right;
        return right;
    }
    if (down == Status::Started)
        return Status::Started;
    return down == Status::Unchanged && right == Status::Unchanged ? Status::Unchanged : Status::Lost;
}

int TwoWayStitcher::length() const
{
    return m_direction == Direction::Right ? m_right.height() : m_down.height();
}

QImage TwoWayStitcher::result() const
{
    return m_direction == Direction::Right ? transposed(m_right.result()) : m_down.result();
}

QImage TwoWayStitcher::tail(int pixels) const
{
    return m_direction == Direction::Right ? transposed(m_right.tail(pixels)) : m_down.tail(pixels);
}
