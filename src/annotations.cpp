#include "annotations.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRegion>

namespace {

QPen strokePen(const AnnotationStyle &style, Qt::PenJoinStyle join = Qt::RoundJoin)
{
    return QPen(style.color, style.width, Qt::SolidLine, Qt::RoundCap, join);
}

// The outline drawn with the mark's pen, widened by tolerance on both sides.
QPainterPath strokeArea(const QPainterPath &outline, qreal width, qreal tolerance)
{
    QPainterPathStroker stroker;
    stroker.setWidth(width + 2 * tolerance);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(outline);
}

// Text color that reads on a solid background of the given color.
QColor contrastingInk(const QColor &background)
{
    return background.lightness() > 160 ? Qt::black : Qt::white;
}

class RectAnnotation : public Annotation
{
public:
    RectAnnotation(const QRectF &rect, const AnnotationStyle &style) : m_rect(rect), m_style(style) {}

    void paint(QPainter &p) const override
    {
        p.setPen(strokePen(m_style, Qt::MiterJoin));
        p.setBrush(m_style.fill ? QBrush(m_style.color) : QBrush(Qt::NoBrush));
        p.drawRect(m_rect);
    }

    QPainterPath hitArea(qreal tolerance) const override
    {
        QPainterPath outline;
        outline.addRect(m_rect);
        const QPainterPath area = strokeArea(outline, m_style.width, tolerance);
        return m_style.fill ? area.united(outline) : area;
    }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<RectAnnotation>(m_rect.translated(delta), m_style);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<RectAnnotation>(m_rect, style);
    }
    QPointF anchor() const override { return m_rect.topLeft(); }
    AnnotationStyle style() const override { return m_style; }

private:
    QRectF m_rect;
    AnnotationStyle m_style;
};

class SpotlightAnnotation : public Annotation
{
public:
    explicit SpotlightAnnotation(const QRectF &rect) : m_rect(rect) {}

    void paint(QPainter &) const override {} // the stack paints one mask for all spotlight areas
    QPainterPath hitArea(qreal tolerance) const override
    {
        QPainterPath outline;
        outline.addRect(m_rect);
        return strokeArea(outline, 1, tolerance);
    }
    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<SpotlightAnnotation>(m_rect.translated(delta));
    }
    AnnotationPtr restyled(const AnnotationStyle &) const override { return {}; }
    QRectF spotlightRect() const override { return m_rect; }
    QPointF anchor() const override { return m_rect.topLeft(); }
    AnnotationStyle style() const override { return {}; }

private:
    QRectF m_rect;
};

class EllipseAnnotation : public Annotation
{
public:
    EllipseAnnotation(const QRectF &rect, const AnnotationStyle &style) : m_rect(rect), m_style(style) {}

    void paint(QPainter &p) const override
    {
        p.setPen(strokePen(m_style));
        p.setBrush(m_style.fill ? QBrush(m_style.color) : QBrush(Qt::NoBrush));
        p.drawEllipse(m_rect);
    }

    QPainterPath hitArea(qreal tolerance) const override
    {
        QPainterPath outline;
        outline.addEllipse(m_rect);
        const QPainterPath area = strokeArea(outline, m_style.width, tolerance);
        return m_style.fill ? area.united(outline) : area;
    }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<EllipseAnnotation>(m_rect.translated(delta), m_style);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<EllipseAnnotation>(m_rect, style);
    }
    QPointF anchor() const override { return m_rect.topLeft(); }
    AnnotationStyle style() const override { return m_style; }

private:
    QRectF m_rect;
    AnnotationStyle m_style;
};

class ArrowAnnotation : public Annotation
{
public:
    // Without a head it is a plain line.
    ArrowAnnotation(const QPointF &from, const QPointF &to, const AnnotationStyle &style, bool head)
        : m_from(from), m_to(to), m_style(style), m_head(head)
    {
    }

    void paint(QPainter &p) const override
    {
        const QLineF line(m_from, m_to);
        if (line.length() < 1)
            return;
        if (!m_head) {
            p.setPen(strokePen(m_style));
            p.drawLine(line);
            return;
        }
        const auto [base, head] = geometry();
        p.setPen(strokePen(m_style));
        p.drawLine(m_from, base);
        p.setPen(Qt::NoPen);
        p.setBrush(m_style.color);
        p.drawPolygon(head);
    }

    QPainterPath hitArea(qreal tolerance) const override
    {
        if (!m_head) {
            QPainterPath line(m_from);
            line.lineTo(m_to);
            return strokeArea(line, m_style.width, tolerance);
        }
        const auto [base, head] = geometry();
        QPainterPath shaft(m_from);
        shaft.lineTo(base);
        QPainterPath area = strokeArea(shaft, m_style.width, tolerance);
        QPainterPath tip;
        tip.addPolygon(head);
        return area.united(tolerance > 0 ? strokeArea(tip, 0, tolerance).united(tip) : tip);
    }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<ArrowAnnotation>(m_from + delta, m_to + delta, m_style, m_head);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<ArrowAnnotation>(m_from, m_to, style, m_head);
    }
    QPointF anchor() const override { return m_from; }
    AnnotationStyle style() const override { return m_style; }

private:
    // Head grows with stroke width but never exceeds the arrow itself.
    std::pair<QPointF, QPolygonF> geometry() const
    {
        const QLineF line(m_from, m_to);
        const qreal headLength = qMin(qMax(12.0, m_style.width * 4), line.length());
        const QLineF unit = line.length() > 0 ? line.unitVector() : QLineF(0, 0, 1, 0);
        const QPointF direction(unit.dx(), unit.dy());
        const QPointF normal(-direction.y(), direction.x());
        const QPointF base = m_to - direction * headLength;
        return {base, QPolygonF{m_to, base + normal * headLength * 0.5, base - normal * headLength * 0.5}};
    }

    QPointF m_from;
    QPointF m_to;
    AnnotationStyle m_style;
    bool m_head;
};

// A measuring line: end ticks and a label with the length in pixels (and the horizontal and
// vertical parts when it is slanted).
class RulerAnnotation : public Annotation
{
public:
    RulerAnnotation(const QPointF &from, const QPointF &to, const AnnotationStyle &style)
        : m_from(from), m_to(to), m_style(style), m_font(annotationFont(style.fontSize * 0.75))
    {
    }

    void paint(QPainter &p) const override
    {
        const QLineF line(m_from, m_to);
        p.setPen(QPen(m_style.color, m_style.width, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(line);
        const QPointF n = normal() * (m_style.width * 2 + 4);
        p.drawLine(m_from - n, m_from + n);
        p.drawLine(m_to - n, m_to + n);

        const QRectF box = labelBox();
        p.setPen(Qt::NoPen);
        p.setBrush(m_style.color);
        p.drawRoundedRect(box, box.height() / 2, box.height() / 2);
        p.setFont(m_font);
        p.setPen(contrastingInk(m_style.color));
        p.drawText(box, Qt::AlignCenter, label());
    }

    QPainterPath hitArea(qreal tolerance) const override
    {
        QPainterPath line(m_from);
        line.lineTo(m_to);
        QPainterPath area = strokeArea(line, m_style.width, tolerance);
        QPainterPath box;
        box.addRect(labelBox().adjusted(-tolerance, -tolerance, tolerance, tolerance));
        return area.united(box);
    }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<RulerAnnotation>(m_from + delta, m_to + delta, m_style);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<RulerAnnotation>(m_from, m_to, style);
    }
    QPointF anchor() const override { return m_from; }
    AnnotationStyle style() const override { return m_style; }

private:
    QPointF normal() const
    {
        const QLineF line(m_from, m_to);
        if (line.length() < 1)
            return QPointF(0, 1);
        const QLineF unit = line.unitVector();
        return QPointF(-unit.dy(), unit.dx());
    }

    QString label() const
    {
        const QLineF line(m_from, m_to);
        QString text = QStringLiteral("%1 px").arg(qRound(line.length()));
        const int dx = qRound(qAbs(line.dx())), dy = qRound(qAbs(line.dy()));
        if (dx > 0 && dy > 0)
            text += QStringLiteral("  (%1 × %2)").arg(dx).arg(dy);
        return text;
    }

    // Beside the middle of the line, on the side away from its normal's direction.
    QRectF labelBox() const
    {
        const QFontMetricsF fm(m_font);
        const QSizeF size(fm.horizontalAdvance(label()) + fm.height(), fm.height() * 1.3);
        const QPointF middle = (m_from + m_to) / 2;
        const QPointF n = normal();
        const QPointF center = middle - n * (size.height() / 2 + m_style.width + 4) * (n.y() > 0 ? 1 : -1);
        return QRectF(center - QPointF(size.width() / 2, size.height() / 2), size);
    }

    QPointF m_from;
    QPointF m_to;
    AnnotationStyle m_style;
    QFont m_font;
};

class StrokeAnnotation : public Annotation
{
public:
    StrokeAnnotation(const QPainterPath &path, const AnnotationStyle &style, bool highlighter)
        : m_path(path), m_style(style), m_highlighter(highlighter)
    {
    }

    void paint(QPainter &p) const override
    {
        p.setBrush(Qt::NoBrush);
        if (m_highlighter) {
            // One path, painted once: where the stroke crosses itself it does not get darker.
            QColor color = m_style.color;
            color.setAlpha(170);
            p.setCompositionMode(QPainter::CompositionMode_Multiply);
            p.setPen(QPen(color, width(), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        } else {
            p.setPen(strokePen(m_style));
        }
        p.drawPath(m_path);
    }

    QPainterPath hitArea(qreal tolerance) const override { return strokeArea(m_path, width(), tolerance); }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<StrokeAnnotation>(m_path.translated(delta), m_style, m_highlighter);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<StrokeAnnotation>(m_path, style, m_highlighter);
    }
    QPointF anchor() const override { return m_path.elementCount() ? QPointF(m_path.elementAt(0)) : QPointF(); }
    AnnotationStyle style() const override { return m_style; }

private:
    // The highlighter follows the text size, so it covers a line of text of that size.
    qreal width() const { return m_highlighter ? m_style.fontSize * 1.2 : m_style.width; }

    QPainterPath m_path;
    AnnotationStyle m_style;
    bool m_highlighter;
};

class TextAnnotation : public Annotation
{
public:
    TextAnnotation(const QPointF &topLeft, const QString &text, const AnnotationStyle &style)
        : m_topLeft(topLeft), m_text(text), m_style(style), m_font(annotationFont(style.fontSize))
    {
    }

    void paint(QPainter &p) const override
    {
        p.setFont(m_font);
        if (m_style.fill) {
            const qreal radius = m_style.fontSize * 0.2;
            p.setPen(Qt::NoPen);
            p.setBrush(m_style.color);
            p.drawRoundedRect(box(), radius, radius);
            p.setPen(contrastingInk(m_style.color));
        } else {
            p.setPen(m_style.color);
        }
        p.drawText(QRectF(m_topLeft, QSizeF(1e6, 1e6)), Qt::AlignLeft | Qt::AlignTop, m_text);
    }

    QPainterPath hitArea(qreal tolerance) const override
    {
        QPainterPath area;
        area.addRect(box().adjusted(-tolerance, -tolerance, tolerance, tolerance));
        return area;
    }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<TextAnnotation>(m_topLeft + delta, m_text, m_style);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<TextAnnotation>(m_topLeft, m_text, style);
    }
    std::optional<QString> text() const override { return m_text; }
    QPointF anchor() const override { return m_topLeft; }
    AnnotationStyle style() const override { return m_style; }

private:
    // The text's extent; with a background, plus the box's padding.
    QRectF box() const
    {
        const QRectF text = QFontMetricsF(m_font).boundingRect(QRectF(m_topLeft, QSizeF(1e6, 1e6)),
                                                               Qt::AlignLeft | Qt::AlignTop, m_text);
        const qreal pad = m_style.fill ? m_style.fontSize * 0.25 : 0;
        return text.adjusted(-pad, -pad * 0.5, pad, pad * 0.5);
    }

    QPointF m_topLeft;
    QString m_text;
    AnnotationStyle m_style;
    QFont m_font;
};

class NumberAnnotation : public Annotation
{
public:
    NumberAnnotation(const QPointF &center, int number, const AnnotationStyle &style)
        : m_center(center), m_number(number), m_style(style)
    {
    }

    int number() const override { return m_number; }

    void paint(QPainter &p) const override
    {
        const QRectF circle = disc();
        p.setPen(Qt::NoPen);
        p.setBrush(m_style.color);
        p.drawEllipse(circle);

        QFont font = annotationFont(m_style.fontSize * 0.9);
        font.setBold(true);
        p.setFont(font);
        p.setPen(contrastingInk(m_style.color));
        p.drawText(circle, Qt::AlignCenter, QString::number(m_number));
    }

    QPainterPath hitArea(qreal tolerance) const override
    {
        QPainterPath area;
        area.addEllipse(disc().adjusted(-tolerance, -tolerance, tolerance, tolerance));
        return area;
    }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        return std::make_shared<NumberAnnotation>(m_center + delta, m_number, m_style);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<NumberAnnotation>(m_center, m_number, style);
    }
    QPointF anchor() const override { return m_center; }
    AnnotationStyle style() const override { return m_style; }

private:
    QRectF disc() const
    {
        const qreal radius = m_style.fontSize * 0.75;
        return QRectF(m_center.x() - radius, m_center.y() - radius, 2 * radius, 2 * radius);
    }

    QPointF m_center;
    int m_number;
    AnnotationStyle m_style;
};

class MosaicAnnotation : public Annotation
{
public:
    // Keeps the (implicitly shared) source so the mosaic can be re-pixelated after a move.
    MosaicAnnotation(const QRect &rect, const QImage &source, const AnnotationStyle &style, bool blur)
        : m_rect(rect), m_source(source), m_style(style), m_blur(blur)
    {
        if (blur) {
            // Blur with some surroundings, so the edges fade like the inside instead of
            // smearing toward the border.
            const int radius = qMax(2, qRound(style.mosaicCell));
            const QRect around = rect.adjusted(-2 * radius, -2 * radius, 2 * radius, 2 * radius) & source.rect();
            m_pixels = blurred(source.copy(around), radius).copy(rect.translated(-around.topLeft()));
        } else {
            // Downscale then upscale without smoothing: each cell becomes one flat average color.
            const int cell = qMax(2, qRound(style.mosaicCell));
            const QImage small = source.copy(rect).scaled(qMax(1, rect.width() / cell), qMax(1, rect.height() / cell),
                                                          Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            m_pixels = small.scaled(rect.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
        }
        m_pixels.setDevicePixelRatio(1);
    }

    void paint(QPainter &p) const override { p.drawImage(m_rect, m_pixels); }

    QPainterPath hitArea(qreal tolerance) const override
    {
        QPainterPath area;
        area.addRect(QRectF(m_rect).adjusted(-tolerance, -tolerance, tolerance, tolerance));
        return area;
    }

    AnnotationPtr moved(const QPointF &delta) const override
    {
        // Pixelates whatever lies under the new place; stays put if it would leave the image.
        const QRect target = m_rect.translated(delta.toPoint());
        if (!m_source.rect().contains(target))
            return std::make_shared<MosaicAnnotation>(*this);
        return std::make_shared<MosaicAnnotation>(target, m_source, m_style, m_blur);
    }
    AnnotationPtr restyled(const AnnotationStyle &style) const override
    {
        return std::make_shared<MosaicAnnotation>(m_rect, m_source, style, m_blur);
    }
    QPointF anchor() const override { return m_rect.topLeft(); }
    AnnotationStyle style() const override { return m_style; }

private:
    QRect m_rect;
    QImage m_source;
    AnnotationStyle m_style;
    bool m_blur;
    QImage m_pixels;
};

// One box blur along rows (or columns when vertical), radius r, edges clamped.
void boxBlur(QImage &image, int r, bool vertical)
{
    const int length = vertical ? image.height() : image.width();
    const int lines = vertical ? image.width() : image.height();
    const int window = 2 * r + 1;
    QList<QRgb> line(length);
    for (int l = 0; l < lines; ++l) {
        auto at = [&](int i) -> QRgb & {
            return reinterpret_cast<QRgb *>(image.scanLine(vertical ? i : l))[vertical ? l : i];
        };
        for (int i = 0; i < length; ++i)
            line[i] = at(i);
        int sum[4] = {};
        auto addPixel = [&](QRgb c, int sign) {
            sum[0] += sign * qRed(c);
            sum[1] += sign * qGreen(c);
            sum[2] += sign * qBlue(c);
            sum[3] += sign * qAlpha(c);
        };
        for (int i = -r; i <= r; ++i)
            addPixel(line[qBound(0, i, length - 1)], 1);
        for (int i = 0; i < length; ++i) {
            at(i) = qRgba(sum[0] / window, sum[1] / window, sum[2] / window, sum[3] / window);
            addPixel(line[qMin(i + r + 1, length - 1)], 1);
            addPixel(line[qMax(i - r, 0)], -1);
        }
    }
}

} // namespace

QFont annotationFont(qreal pixelSize)
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(qMax(1, qRound(pixelSize)));
    return font;
}

AnnotationPtr makeShape(Tool tool, const QPoint &from, const QPoint &to, const AnnotationStyle &style)
{
    const QRectF rect = QRectF(QPointF(from), QPointF(to)).normalized();
    switch (tool) {
    case Tool::Rect:
        return std::make_shared<RectAnnotation>(rect, style);
    case Tool::Ellipse:
        return std::make_shared<EllipseAnnotation>(rect, style);
    case Tool::Line:
    case Tool::Arrow:
        return std::make_shared<ArrowAnnotation>(from, to, style, tool == Tool::Arrow);
    case Tool::Ruler:
        return std::make_shared<RulerAnnotation>(from, to, style);
    default:
        return nullptr;
    }
}

AnnotationPtr makeSpotlight(const QRectF &rect)
{
    if (rect.isEmpty())
        return {};
    return std::make_shared<SpotlightAnnotation>(rect);
}

AnnotationPtr makeStroke(const QPolygonF &points, const AnnotationStyle &style, bool highlighter)
{
    QPainterPath path;
    path.addPolygon(points);
    return std::make_shared<StrokeAnnotation>(path, style, highlighter);
}

AnnotationPtr makeText(const QPoint &topLeft, const QString &text, const AnnotationStyle &style)
{
    return std::make_shared<TextAnnotation>(topLeft, text, style);
}

AnnotationPtr makeNumber(const QPoint &center, int number, const AnnotationStyle &style)
{
    return std::make_shared<NumberAnnotation>(center, number, style);
}

AnnotationPtr makeMosaic(const QRect &rect, const QImage &source, const AnnotationStyle &style, bool blur)
{
    const QRect clipped = rect & source.rect();
    if (clipped.isEmpty())
        return nullptr;
    return std::make_shared<MosaicAnnotation>(clipped, source, style, blur);
}

QImage blurred(const QImage &image, qreal radius)
{
    if (image.isNull() || radius < 1)
        return image;
    // Blurring a reduced copy keeps big regions fast enough to follow the mouse; three box
    // passes approximate a Gaussian. Radius 3 on the reduced copy; the factor makes up the rest.
    constexpr int kSmallRadius = 3;
    const qreal factor = qMax(1.0, radius / kSmallRadius);
    const QSize smallSize(qMax(1, qRound(image.width() / factor)), qMax(1, qRound(image.height() / factor)));
    QImage small = image.convertToFormat(QImage::Format_ARGB32)
                       .scaled(smallSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int r = qMax(1, qRound(radius / factor));
    for (int pass = 0; pass < 3; ++pass) {
        boxBlur(small, r, false);
        boxBlur(small, r, true);
    }
    return small.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(image.format());
}

int AnnotationStack::nextNumber() const
{
    int last = 0;
    for (const AnnotationPtr &item : m_items)
        last = qMax(last, item->number());
    return last + 1;
}

int AnnotationStack::itemAt(const QPointF &pos, qreal tolerance) const
{
    for (int i = int(m_items.size()) - 1; i >= 0; --i) {
        if (m_items[i]->hitArea(tolerance).contains(pos))
            return i;
    }
    return -1;
}

void AnnotationStack::add(const AnnotationPtr &item)
{
    if (!item)
        return;
    if (!m_editStart) {
        m_undo.append(m_items);
        m_redo.clear();
    }
    m_items.append(item);
}

void AnnotationStack::remove(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    if (!m_editStart) {
        m_undo.append(m_items);
        m_redo.clear();
    }
    m_items.removeAt(index);
}

void AnnotationStack::undo()
{
    if (m_undo.isEmpty())
        return;
    m_redo.append(m_items);
    m_items = m_undo.takeLast();
}

void AnnotationStack::redo()
{
    if (m_redo.isEmpty())
        return;
    m_undo.append(m_items);
    m_items = m_redo.takeLast();
}

void AnnotationStack::clear()
{
    m_items.clear();
    m_undo.clear();
    m_redo.clear();
    m_editStart.reset();
}

void AnnotationStack::beginEdit()
{
    m_editStart = m_items;
}

void AnnotationStack::setItem(int index, const AnnotationPtr &item)
{
    if (index >= 0 && index < m_items.size() && item)
        m_items[index] = item;
}

void AnnotationStack::commitEdit()
{
    if (m_editStart && *m_editStart != m_items) {
        m_undo.append(*m_editStart);
        m_redo.clear();
    }
    m_editStart.reset();
}

void AnnotationStack::cancelEdit()
{
    if (m_editStart)
        m_items = *m_editStart;
    m_editStart.reset();
}

void AnnotationStack::paint(QPainter &p, const QRect &bounds, const AnnotationPtr &draft) const
{
    QRegion focus;
    for (const AnnotationPtr &item : m_items)
        focus += item->spotlightRect().toRect();
    if (draft)
        focus += draft->spotlightRect().toRect();
    if (!focus.isEmpty()) {
        p.save();
        p.setCompositionMode(QPainter::CompositionMode_SourceAtop); // preserve a freeform crop's alpha
        for (const QRect &dimmed : QRegion(bounds).subtracted(focus))
            p.fillRect(dimmed, QColor(0, 0, 0, 120));
        p.restore();
    }

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    for (const AnnotationPtr &item : m_items) {
        p.save();
        item->paint(p);
        p.restore();
    }
    if (draft)
        draft->paint(p);
    p.restore();
}
