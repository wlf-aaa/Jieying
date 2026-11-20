#include "pinwindow.h"

#include "icons.h"
#include "imageio.h"

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QMimeData>
#include <QWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtMath>

namespace {

constexpr int kShadow = 10;       // logical px of soft shadow around the image
constexpr qreal kMinScale = 0.1;
constexpr qreal kMaxScale = 10.0;
constexpr qreal kZoomStep = 1.1;
const QColor kAccent(30, 144, 255);

} // namespace

PinWindow::PinWindow(const QPixmap &image, const QPoint &contentTopLeft)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    , m_image(image)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SizeAllCursor);
    setAcceptDrops(true);

    m_hintTimer.setSingleShot(true);
    m_hintTimer.setInterval(900);
    connect(&m_hintTimer, &QTimer::timeout, this, [this] {
        m_hint.clear();
        update();
    });

    resize(contentSize() + QSize(2 * kShadow, 2 * kShadow));
    move(contentTopLeft - QPoint(kShadow, kShadow));
}

QSize PinWindow::contentSize() const
{
    const QSizeF size = m_image.deviceIndependentSize() * m_scale;
    return QSize(qMax(1, qRound(size.width())), qMax(1, qRound(size.height())));
}

void PinWindow::setScale(qreal scale, const QPointF &anchorGlobal)
{
    scale = qBound(kMinScale, scale, kMaxScale);
    if (qFuzzyCompare(scale, m_scale))
        return;

    // Keep the image point under the anchor fixed while zooming.
    const QPointF contentTopLeft = pos() + QPointF(kShadow, kShadow);
    const QSizeF oldSize = contentSize();
    const QPointF fraction((anchorGlobal.x() - contentTopLeft.x()) / oldSize.width(),
                           (anchorGlobal.y() - contentTopLeft.y()) / oldSize.height());
    m_scale = scale;
    const QSize newSize = contentSize();
    const QPointF newTopLeft(anchorGlobal.x() - fraction.x() * newSize.width(),
                             anchorGlobal.y() - fraction.y() * newSize.height());

    setGeometry(QRect(newTopLeft.toPoint() - QPoint(kShadow, kShadow), newSize + QSize(2 * kShadow, 2 * kShadow)));
    showHint(tr("缩放 %1%").arg(qRound(m_scale * 100)));
}

QRect PinWindow::prepareForEditing()
{
    setScale(1.0, pos() + QPointF(kShadow, kShadow)); // keep the image's top-left in place
    setWindowOpacity(1.0);
    return QRect(pos() + QPoint(kShadow, kShadow), contentSize());
}

void PinWindow::replaceImage(const QPixmap &image)
{
    const QPoint contentTopLeft = pos() + QPoint(kShadow, kShadow);
    m_image = image;
    m_scale = 1.0;
    setGeometry(QRect(contentTopLeft - QPoint(kShadow, kShadow), contentSize() + QSize(2 * kShadow, 2 * kShadow)));
    update();
}

void PinWindow::setImageKeepingCenter(const QPixmap &image)
{
    const QPoint center = geometry().center();
    m_image = image;
    QRect frame(QPoint(), contentSize() + QSize(2 * kShadow, 2 * kShadow));
    frame.moveCenter(center);
    setGeometry(frame);
    update();
}

void PinWindow::rotate(int degrees)
{
    QPixmap turned = m_image.transformed(QTransform().rotate(degrees));
    turned.setDevicePixelRatio(m_image.devicePixelRatio());
    setImageKeepingCenter(turned);
}

void PinWindow::flip(bool horizontal)
{
    QImage mirrored = m_image.toImage().flipped(horizontal ? Qt::Horizontal : Qt::Vertical);
    QPixmap flipped = QPixmap::fromImage(mirrored);
    flipped.setDevicePixelRatio(m_image.devicePixelRatio());
    setImageKeepingCenter(flipped);
}

void PinWindow::beginCrop()
{
    m_cropping = true;
    m_cropRect = QRect();
    setCursor(Qt::CrossCursor);
    showHint(tr("拖动框选要保留的部分 · Enter 确认 · Esc 取消"));
}

void PinWindow::cropTo(const QRect &contentRect)
{
    const QRect content(kShadow, kShadow, width() - 2 * kShadow, height() - 2 * kShadow);
    const QRect kept = contentRect & content;
    if (kept.width() < 2 || kept.height() < 2)
        return;
    // Window pixels -> image pixels: undo the zoom, then apply the image's pixel ratio.
    const qreal toImage = m_image.devicePixelRatio() / m_scale;
    const QRect source = QRectF((kept.left() - kShadow) * toImage, (kept.top() - kShadow) * toImage,
                                kept.width() * toImage, kept.height() * toImage)
                             .toAlignedRect()
                             & m_image.rect();
    const QPoint keptOnScreen = mapToGlobal(kept.topLeft());
    QPixmap cropped = m_image.copy(source);
    cropped.setDevicePixelRatio(m_image.devicePixelRatio());
    m_image = cropped;
    setGeometry(QRect(keptOnScreen - QPoint(kShadow, kShadow), contentSize() + QSize(2 * kShadow, 2 * kShadow)));
    update();
}

void PinWindow::setClickThrough(bool on)
{
    if (on == m_clickThrough)
        return;
    m_clickThrough = on;
    m_dragging = false;
    // Changing window flags recreates the native window; it has to be shown again.
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowTransparentForInput, on);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, on);
    if (visible)
        show();
    showHint(on ? tr("鼠标穿透已开启 · 托盘菜单可恢复") : tr("鼠标穿透已关闭"));
    emit clickThroughChanged(on);
}

void PinWindow::setOpacity(qreal opacity)
{
    opacity = qBound(0.1, opacity, 1.0);
    setWindowOpacity(opacity);
    showHint(tr("不透明度 %1%").arg(qRound(opacity * 100)));
}

void PinWindow::showHint(const QString &text)
{
    m_hint = text;
    m_hintTimer.start();
    update();
}

void PinWindow::save()
{
    ImageIO::saveImage(m_image, this);
}

void PinWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRectF content(kShadow, kShadow, width() - 2 * kShadow, height() - 2 * kShadow);

    // Soft shadow: outlines growing darker toward the image.
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    for (int d = kShadow; d >= 1; --d) {
        const qreal t = 1.0 - qreal(d) / kShadow;
        p.setPen(QPen(QColor(0, 0, 0, qRound(6 + 40 * t * t)), 1));
        p.drawRoundedRect(content.adjusted(-d + 0.5, -d + 0.5, d - 0.5, d - 0.5), d, d);
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    p.setRenderHint(QPainter::SmoothPixmapTransform, !qFuzzyCompare(m_scale, 1.0));
    p.drawPixmap(content, m_image, QRectF(m_image.rect()));

    // Click-through pins get a dashed edge: they look pinned but are not grabbable.
    p.setPen(QPen(hasFocus() ? kAccent : QColor(128, 128, 128, 140), 1, m_clickThrough ? Qt::DashLine : Qt::SolidLine));
    p.drawRect(content.adjusted(-0.5, -0.5, 0.5, 0.5));

    if (m_cropping && !m_cropRect.isEmpty()) {
        // Dim what will be cut away.
        QPainterPath outside;
        outside.addRect(content);
        QPainterPath keep;
        keep.addRect(QRectF(m_cropRect));
        p.fillPath(outside.subtracted(keep), QColor(0, 0, 0, 120));
        p.setPen(QPen(kAccent, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(m_cropRect).adjusted(0.5, 0.5, -0.5, -0.5));
    }

    if (!m_hint.isEmpty()) {
        const QFontMetricsF fm(font());
        QRectF box(0, 0, fm.horizontalAdvance(m_hint) + 16, fm.height() + 8);
        box.moveCenter(content.center());
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, m_hint);
    }
}

void PinWindow::mousePressEvent(QMouseEvent *event)
{
    if (m_cropping && event->button() == Qt::LeftButton) {
        m_cropStart = event->position().toPoint();
        m_cropRect = QRect();
        update();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - pos();
    }
}

void PinWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_cropping && (event->buttons() & Qt::LeftButton)) {
        const QRect content(kShadow, kShadow, width() - 2 * kShadow, height() - 2 * kShadow);
        const QPoint end = event->position().toPoint();
        const QPoint topLeft(qMin(m_cropStart.x(), end.x()), qMin(m_cropStart.y(), end.y()));
        m_cropRect = QRect(topLeft, QSize(qAbs(end.x() - m_cropStart.x()), qAbs(end.y() - m_cropStart.y()))) & content;
        update();
        return;
    }
    if (m_dragging)
        move(event->globalPosition().toPoint() - m_dragOffset);
}

void PinWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        m_dragging = false;
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_cropping) { // double-click confirms, like Enter
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        keyPressEvent(&enter);
        return;
    }
    close();
}

void PinWindow::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0)
        return;
    if (event->modifiers() & Qt::ControlModifier)
        setOpacity(windowOpacity() + (delta > 0 ? 0.1 : -0.1));
    else
        setScale(delta > 0 ? m_scale * kZoomStep : m_scale / kZoomStep, event->globalPosition());
}

void PinWindow::keyPressEvent(QKeyEvent *event)
{
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    if (m_cropping) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Escape) {
            if (event->key() != Qt::Key_Escape && !m_cropRect.isEmpty())
                cropTo(m_cropRect);
            m_cropping = false;
            m_cropRect = QRect();
            setCursor(Qt::SizeAllCursor);
            update();
        }
        return;
    }
    if (!ctrl && event->key() == Qt::Key_X) {
        beginCrop();
    } else if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Delete) {
        close();
    } else if (!ctrl && event->key() == Qt::Key_R) {
        rotate(shift ? -90 : 90);
    } else if (!ctrl && event->key() == Qt::Key_H) {
        flip(true);
    } else if (!ctrl && event->key() == Qt::Key_V) {
        flip(false);
    } else if ((!ctrl && event->key() == Qt::Key_P) || event->key() == Qt::Key_F4) {
        setClickThrough(true);
    } else if (ctrl && (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)) {
        setOpacity(windowOpacity() + 0.1);
    } else if (ctrl && event->key() == Qt::Key_Minus) {
        setOpacity(windowOpacity() - 0.1);
    } else if (event->matches(QKeySequence::Copy)) {
        ImageIO::copyToClipboard(m_image);
        showHint(tr("已复制"));
    } else if (event->matches(QKeySequence::Save)) {
        save();
    } else if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_E) {
        emit annotateRequested();
    } else if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_0) {
        setScale(1.0, mapToGlobal(rect().center()));
    } else {
        QWidget::keyPressEvent(event);
    }
}

void PinWindow::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(toolIcon("copy"), tr("复制"), QKeySequence::Copy, this, [this] {
        ImageIO::copyToClipboard(m_image);
        showHint(tr("已复制"));
    });
    menu.addAction(toolIcon("save"), tr("另存为…"), QKeySequence::Save, this, &PinWindow::save);
    menu.addAction(toolIcon("pen"), tr("标注…"), QKeySequence(Qt::CTRL | Qt::Key_E), this,
                   &PinWindow::annotateRequested);
    menu.addAction(toolIcon("ocr"), tr("识别文字"), this, [this] { emit ocrRequested(m_image); });
    menu.addAction(toolIcon("beautify"), tr("美化…"), this, [this] { emit beautifyRequested(m_image); });
    menu.addAction(toolIcon("actual-size"), tr("原始大小"), QKeySequence(Qt::CTRL | Qt::Key_0), this,
                   [this] { setScale(1.0, mapToGlobal(rect().center())); });

    menu.addAction(tr("裁剪…"), QKeySequence(Qt::Key_X), this, &PinWindow::beginCrop);
    QMenu *turn = menu.addMenu(tr("旋转 / 翻转"));
    turn->addAction(tr("顺时针旋转 90°"), QKeySequence(Qt::Key_R), this, [this] { rotate(90); });
    turn->addAction(tr("逆时针旋转 90°"), QKeySequence(Qt::SHIFT | Qt::Key_R), this, [this] { rotate(-90); });
    turn->addAction(tr("水平翻转"), QKeySequence(Qt::Key_H), this, [this] { flip(true); });
    turn->addAction(tr("垂直翻转"), QKeySequence(Qt::Key_V), this, [this] { flip(false); });
    menu.addAction(tr("鼠标穿透（托盘菜单恢复）"), QKeySequence(Qt::Key_P), this, [this] { setClickThrough(true); });

    QMenu *opacityMenu = menu.addMenu(toolIcon("opacity"), tr("不透明度"));
    auto *group = new QActionGroup(opacityMenu);
    for (int percent : {100, 80, 60, 40}) {
        QAction *action = opacityMenu->addAction(QStringLiteral("%1%").arg(percent),
                                                 this, [this, percent] { setOpacity(percent / 100.0); });
        action->setCheckable(true);
        action->setChecked(qRound(windowOpacity() * 100) == percent);
        group->addAction(action);
    }

    emit menuAboutToShow(&menu);

    menu.addSeparator();
    menu.addAction(toolIcon("close"), tr("关闭"), QKeySequence(Qt::Key_Escape), this, &QWidget::close);
    menu.exec(event->globalPos());
}

void PinWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage())
        event->acceptProposedAction();
}

void PinWindow::dropEvent(QDropEvent *event)
{
    const QList<QImage> images = ImageIO::pinnableImages(event->mimeData(), m_image.devicePixelRatio());
    if (images.isEmpty()) {
        showHint(tr("不是图片"));
        return;
    }
    event->acceptProposedAction();
    emit imagesDropped(images, mapToGlobal(event->position().toPoint()));
}

void PinWindow::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    update();
}

void PinWindow::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    update();
}
