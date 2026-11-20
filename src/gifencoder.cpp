#include "frameencoder.h"

#include <QPainter>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

int rgb15(QRgb c)
{
    return ((qRed(c) >> 3) << 10) | ((qGreen(c) >> 3) << 5) | (qBlue(c) >> 3);
}

int channel(int color15, int index)
{
    return (color15 >> (10 - 5 * index)) & 31;
}

void putU16(QByteArray &out, int value)
{
    out.append(char(value & 0xFF));
    out.append(char((value >> 8) & 0xFF));
}

// Bounding box of pixels that differ between two same-sized images; empty if identical.
QRect changedRect(const QImage &a, const QImage &b)
{
    int left = a.width(), right = -1, top = a.height(), bottom = -1;
    for (int y = 0; y < a.height(); ++y) {
        const auto *ra = reinterpret_cast<const quint32 *>(a.constScanLine(y));
        const auto *rb = reinterpret_cast<const quint32 *>(b.constScanLine(y));
        if (std::memcmp(ra, rb, size_t(a.width()) * 4) == 0)
            continue;
        top = qMin(top, y);
        bottom = y;
        for (int x = 0; x < a.width(); ++x) {
            if (ra[x] != rb[x]) {
                left = qMin(left, x);
                break;
            }
        }
        for (int x = a.width() - 1; x >= 0; --x) {
            if (ra[x] != rb[x]) {
                right = qMax(right, x);
                break;
            }
        }
    }
    return bottom < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

} // namespace

namespace Gif {

Quantized quantize(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    std::vector<int> histogram(1 << 15, 0);
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            ++histogram[rgb15(row[x])];
    }
    std::vector<int> colors;
    for (int c = 0; c < int(histogram.size()); ++c) {
        if (histogram[c])
            colors.push_back(c);
    }

    // Median cut: repeatedly split the box with the widest channel range at its weighted median.
    struct Box
    {
        int begin, end;
    };
    std::vector<Box> boxes{{0, int(colors.size())}};
    auto widest = [&](const Box &box, int &axis) {
        int best = -1;
        for (int ch = 0; ch < 3; ++ch) {
            int lo = 31, hi = 0;
            for (int i = box.begin; i < box.end; ++i) {
                lo = qMin(lo, channel(colors[i], ch));
                hi = qMax(hi, channel(colors[i], ch));
            }
            if (hi - lo > best) {
                best = hi - lo;
                axis = ch;
            }
        }
        return best;
    };
    while (boxes.size() < 256) {
        int pick = -1, axis = 0, pickRange = 0;
        for (int i = 0; i < int(boxes.size()); ++i) {
            if (boxes[i].end - boxes[i].begin < 2)
                continue;
            int boxAxis = 0;
            const int range = widest(boxes[i], boxAxis);
            if (range > pickRange) {
                pickRange = range;
                pick = i;
                axis = boxAxis;
            }
        }
        if (pick < 0)
            break;
        Box &box = boxes[pick];
        std::sort(colors.begin() + box.begin, colors.begin() + box.end,
                  [axis](int a, int b) { return channel(a, axis) < channel(b, axis); });
        long long total = 0;
        for (int i = box.begin; i < box.end; ++i)
            total += histogram[colors[i]];
        long long running = 0;
        int split = box.begin + 1;
        for (int i = box.begin; i < box.end - 1; ++i) {
            running += histogram[colors[i]];
            split = i + 1;
            if (running * 2 >= total)
                break;
        }
        const Box upper{split, box.end};
        box.end = split;
        boxes.push_back(upper);
    }

    Quantized result;
    std::vector<uchar> lookup(1 << 15, 0);
    for (int b = 0; b < int(boxes.size()); ++b) {
        long long r = 0, g = 0, bl = 0, weight = 0;
        for (int i = boxes[b].begin; i < boxes[b].end; ++i) {
            const int c = colors[i];
            const int n = histogram[c];
            r += ((channel(c, 0) << 3) | (channel(c, 0) >> 2)) * n;
            g += ((channel(c, 1) << 3) | (channel(c, 1) >> 2)) * n;
            bl += ((channel(c, 2) << 3) | (channel(c, 2) >> 2)) * n;
            weight += n;
            lookup[c] = uchar(b);
        }
        weight = qMax(1LL, weight);
        result.palette.append(qRgb(int(r / weight), int(g / weight), int(bl / weight)));
    }

    result.indices.resize(qsizetype(image.width()) * image.height());
    char *out = result.indices.data();
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            *out++ = char(lookup[rgb15(row[x])]);
    }
    return result;
}

QByteArray lzwCompress(const QByteArray &indices)
{
    constexpr int kMinCodeSize = 8;
    constexpr int kClear = 1 << kMinCodeSize;
    constexpr int kEnd = kClear + 1;

    QByteArray packed;
    quint32 bits = 0;
    int bitCount = 0;
    int codeSize = kMinCodeSize + 1;
    auto emitCode = [&](int code) {
        bits |= quint32(code) << bitCount;
        bitCount += codeSize;
        while (bitCount >= 8) {
            packed.append(char(bits & 0xFF));
            bits >>= 8;
            bitCount -= 8;
        }
    };

    // dictionary[prefix * 256 + byte] = code; entries are reset on every clear.
    std::vector<short> dictionary(4096 * 256, -1);
    std::vector<int> used;
    used.reserve(4096);
    int maxCode = kEnd;

    emitCode(kClear);
    if (!indices.isEmpty()) {
        int prefix = uchar(indices[0]);
        for (qsizetype i = 1; i < indices.size(); ++i) {
            const int next = uchar(indices[i]);
            const int key = prefix * 256 + next;
            if (dictionary[key] >= 0) {
                prefix = dictionary[key];
                continue;
            }
            emitCode(prefix);
            dictionary[key] = short(++maxCode);
            used.push_back(key);
            if (maxCode >= (1 << codeSize))
                ++codeSize;
            if (maxCode == 4095) { // table full: start over
                emitCode(kClear);
                for (int k : used)
                    dictionary[k] = -1;
                used.clear();
                codeSize = kMinCodeSize + 1;
                maxCode = kEnd;
            }
            prefix = next;
        }
        emitCode(prefix);
    }
    emitCode(kEnd);
    if (bitCount > 0)
        packed.append(char(bits & 0xFF));

    QByteArray blocks;
    blocks.append(char(kMinCodeSize));
    for (qsizetype i = 0; i < packed.size(); i += 255) {
        const qsizetype n = qMin<qsizetype>(255, packed.size() - i);
        blocks.append(char(n));
        blocks.append(packed.constData() + i, n);
    }
    blocks.append(char(0));
    return blocks;
}

} // namespace Gif

bool GifEncoder::open(const QString &path, const QSize &size, int)
{
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(m_file.errorString());
    m_size = size;
    m_shown = QImage();
    m_pending = QImage();
    m_writtenCs = 0;

    QByteArray header("GIF89a");
    putU16(header, size.width());
    putU16(header, size.height());
    header.append(char(0)).append(char(0)).append(char(0)); // no global palette
    // NETSCAPE2.0 application extension: loop forever.
    header.append("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
    return m_file.write(header) == header.size() || fail(m_file.errorString());
}

bool GifEncoder::addFrame(const QImage &input, qint64 timestampMs)
{
    QImage frame = input.convertToFormat(QImage::Format_RGB32);
    if (frame.size() != m_size)
        frame = frame.copy(QRect(QPoint(0, 0), m_size));
    if (!m_pending.isNull()) {
        if (changedRect(frame, m_pending).isEmpty())
            return true; // identical: the pending frame just stays up longer
        if (!writePending(timestampMs))
            return false;
    }
    m_pending = frame;
    m_pendingMs = timestampMs;
    return true;
}

bool GifEncoder::writePending(qint64 untilMs)
{
    // Delays are in centiseconds; derive each from the running total so rounding never drifts.
    const qint64 endCs = qMax(m_writtenCs + 2, (untilMs + 5) / 10);
    const int delay = int(qMin<qint64>(endCs - m_writtenCs, 65535));
    m_writtenCs += delay;

    const QRect rect = m_shown.isNull() ? QRect(QPoint(0, 0), m_size) : changedRect(m_pending, m_shown);
    QByteArray out;
    // Graphic control extension: keep the previous frame underneath (disposal 1), set delay.
    out.append("\x21\xF9\x04\x04", 4);
    putU16(out, delay);
    out.append(char(0)).append(char(0));

    if (rect.isEmpty()) { // only the timing changed: emit a 1x1 frame identical to what is shown
        const QImage pixel = m_pending.copy(0, 0, 1, 1);
        out.append(char(0x2C));
        putU16(out, 0);
        putU16(out, 0);
        putU16(out, 1);
        putU16(out, 1);
        out.append('\x80'); // local palette with 2 entries
        out.append(char(qRed(pixel.pixel(0, 0)))).append(char(qGreen(pixel.pixel(0, 0)))).append(char(qBlue(pixel.pixel(0, 0))));
        out.append(QByteArray(3, 0));
        out.append(Gif::lzwCompress(QByteArray(1, 0)));
    } else {
        const Gif::Quantized q = Gif::quantize(m_pending.copy(rect));
        out.append(char(0x2C));
        putU16(out, rect.x());
        putU16(out, rect.y());
        putU16(out, rect.width());
        putU16(out, rect.height());
        out.append('\x87'); // local palette, 256 entries
        for (int i = 0; i < 256; ++i) {
            const QRgb c = i < q.palette.size() ? q.palette[i] : 0;
            out.append(char(qRed(c))).append(char(qGreen(c))).append(char(qBlue(c)));
        }
        out.append(Gif::lzwCompress(q.indices));
    }
    if (m_file.write(out) != out.size())
        return fail(m_file.errorString());
    m_shown = m_pending;
    return true;
}

bool GifEncoder::finish(qint64 endMs)
{
    if (!m_pending.isNull() && !writePending(endMs))
        return false;
    m_pending = QImage();
    if (m_file.write("\x3B", 1) != 1)
        return fail(m_file.errorString());
    m_file.close();
    return true;
}
