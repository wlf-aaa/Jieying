#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QPainterPath>
#include <QPolygonF>
#include <QRect>
#include <QString>

#include <memory>
#include <optional>

class QPainter;

enum class Tool { None, Rect, Ellipse, Line, Arrow, Pen, Highlighter, Text, Mosaic, Blur, Number, Eraser, Ruler };

// Sizes are in the same physical pixels as the annotation geometry.
struct AnnotationStyle
{
    QColor color;
    qreal width;
    qreal fontSize;
    qreal mosaicCell;
    bool fill = false; // solid rectangle/ellipse, background box behind text
};

class Annotation;
using AnnotationPtr = std::shared_ptr<const Annotation>;

// An immutable mark on the screenshot, positioned in screen-local physical pixels.
// Editing produces a modified copy (moved(), restyled()), which keeps undo snapshots cheap.
class Annotation
{
public:
    virtual ~Annotation() = default;
    virtual void paint(QPainter &p) const = 0;

    // Area that counts as "on" the mark: the stroke widened by tolerance for outlines,
    // the body for filled marks (text, numbers, mosaic).
    virtual QPainterPath hitArea(qreal tolerance) const = 0;
    QRectF bounds() const { return hitArea(0).boundingRect(); }

    virtual AnnotationPtr moved(const QPointF &delta) const = 0;
    // Same mark with another color/width/font size (mosaic: another cell size).
    virtual AnnotationPtr restyled(const AnnotationStyle &style) const = 0;

    virtual int number() const { return 0; }                   // non-zero for numbered markers
    virtual std::optional<QString> text() const { return {}; } // text marks only
    virtual QPointF anchor() const = 0;                        // top-left for text, origin otherwise
    virtual AnnotationStyle style() const = 0;
};

// Rect, Ellipse, Line, Arrow or Ruler (a measured line labelled with its length) from -> to.
AnnotationPtr makeShape(Tool tool, const QPoint &from, const QPoint &to, const AnnotationStyle &style);
// A pen stroke; the highlighter is a wide translucent one that multiplies onto the image,
// so dark text under it stays dark.
AnnotationPtr makeStroke(const QPolygonF &points, const AnnotationStyle &style, bool highlighter = false);
AnnotationPtr makeText(const QPoint &topLeft, const QString &text, const AnnotationStyle &style);
AnnotationPtr makeNumber(const QPoint &center, int number, const AnnotationStyle &style);
// Pixelates (or blurs) rect of source; returns null for an empty rect.
AnnotationPtr makeMosaic(const QRect &rect, const QImage &source, const AnnotationStyle &style, bool blur = false);

// Gaussian-like blur of an image (three box-blur passes on a reduced copy). Exposed for tests.
QImage blurred(const QImage &image, qreal radius);

// Font shared by text annotations and the inline text editor so both lay out alike.
QFont annotationFont(qreal pixelSize);

// Ordered annotations with snapshot undo/redo. Annotations are immutable,
// so a snapshot is just a list of shared pointers.
class AnnotationStack
{
public:
    bool isEmpty() const { return m_items.isEmpty(); }
    int size() const { return int(m_items.size()); }
    const AnnotationPtr &at(int index) const { return m_items[index]; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    int nextNumber() const;

    // Topmost annotation whose hit area contains pos, or -1.
    int itemAt(const QPointF &pos, qreal tolerance) const;

    // Inside beginEdit()/commitEdit(), add and remove join that edit's single undo step
    // (the eraser, one-click redaction).
    void add(const AnnotationPtr &item);
    void remove(int index);
    void undo();
    void redo();
    void clear();

    // An edit spanning many updates (dragging) becomes one undo step: beginEdit() remembers the
    // current state, setItem() changes items freely, commitEdit() records the step if anything
    // changed and cancelEdit() drops it.
    void beginEdit();
    void setItem(int index, const AnnotationPtr &item);
    void commitEdit();
    void cancelEdit();

    // Paints every annotation, then draft (an in-progress one) on top if given.
    void paint(QPainter &p, const AnnotationPtr &draft = nullptr) const;

private:
    QList<AnnotationPtr> m_items;
    QList<QList<AnnotationPtr>> m_undo;
    QList<QList<AnnotationPtr>> m_redo;
    std::optional<QList<AnnotationPtr>> m_editStart;
};
