#include "selectionoverlay.h"

#include "config.h"
#include "sensitive.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScreen>
#include <QWindow>
#include <QtConcurrent>
#include <QtMath>

#include <array>
#include <cmath>

namespace {

const QColor kAccent(30, 144, 255);
const QColor kDim(0, 0, 0, 110);
constexpr qreal kEdgeTolerance = 6.0; // logical px around the selection border that grab a resize
constexpr qreal kHandleSize = 6.0;
constexpr int kDragThreshold = 4;     // physical px a press must travel to become a drag
constexpr int kMagnifierSource = 15;  // odd, so the cursor's pixel sits in the middle
constexpr int kMagnifierZoom = 8;

// Per size level, in logical px.
constexpr qreal kStrokeWidths[] = {2, 4, 8};
constexpr qreal kFontSizes[] = {14, 20, 30};
constexpr qreal kMosaicCells[] = {6, 10, 16};

std::array<QPointF, 8> handleCenters(const QRectF &r)
{
    return {r.topLeft(), QPointF(r.center().x(), r.top()), r.topRight(), QPointF(r.right(), r.center().y()),
            r.bottomRight(), QPointF(r.center().x(), r.bottom()), r.bottomLeft(), QPointF(r.left(), r.center().y())};
}

QRect toolbarGeometry(const CaptureToolbar &toolbar, const QRect &bounds, const QRect &selection)
{
    constexpr int kGap = 6;
    const auto positionIn = [&](const QRect &space, const QSize &size) {
        const int maxX = qMax(space.left(), space.right() + 1 - size.width());
        const int maxY = qMax(space.top(), space.bottom() + 1 - size.height());
        const int x = qBound(space.left(), selection.right() + 1 - size.width(), maxX);
        const int y = qBound(space.top(), selection.bottom() + 1 - kGap - size.height(), maxY);
        return QRect(QPoint(x, y), size);
    };

    // Prefer a full-width strip underneath or above, then try wrapping beside the selection.
    for (int gap : {kGap, 0}) {
        QRect below = bounds;
        below.setTop(qMax(bounds.top(), selection.bottom() + 1 + gap));
        QRect above = bounds;
        above.setBottom(qMin(bounds.bottom(), selection.top() - 1 - gap));
        QRect right = bounds;
        right.setLeft(qMax(bounds.left(), selection.right() + 1 + gap));
        QRect left = bounds;
        left.setRight(qMin(bounds.right(), selection.left() - 1 - gap));

        for (const QRect &space : {below, above, right, left}) {
            if (space.isEmpty())
                continue;
            const QSize size = toolbar.sizeForWidth(space.width());
            if (size.width() <= space.width() && size.height() <= space.height())
                return positionIn(space, size);
        }
    }
    return positionIn(bounds, toolbar.sizeForWidth(bounds.width()));
}

} // namespace

SelectionOverlay::SelectionOverlay(QScreen *screen, const QPixmap &shot, const QList<WindowNode> &windows)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    , m_screen(screen)
    , m_showMagnifier(Config::showMagnifier())
    , m_confirmAction(configuredConfirmAction())
    , m_dpr(screen->devicePixelRatio())
    , m_shot(shot)
    , m_image(shot.toImage())
{
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    m_shot.setDevicePixelRatio(m_dpr);

    // Qt keeps a screen's logical top-left equal to its native one, so the logical origin
    // translates global physical window rects into this screen's physical space.
    const QPoint origin = screen->geometry().topLeft();
    const QRect bounds = m_image.rect();
    for (WindowNode node : windows) {
        node.rect = node.rect.translated(-origin) & bounds;
        if (node.rect.isEmpty())
            continue;
        QList<QRect> children;
        for (const QRect &child : std::as_const(node.children)) {
            const QRect local = child.translated(-origin) & node.rect;
            if (!local.isEmpty())
                children.append(local);
        }
        node.children = children;
        m_windows.append(node);
    }

    buildToolbar();

    create();
    windowHandle()->setScreen(screen);
    setGeometry(screen->geometry());
}

void SelectionOverlay::buildToolbar()
{
    m_toolbar = new CaptureToolbar(this);
    m_toolbar->hide();
    connect(m_toolbar, &CaptureToolbar::toolChanged, this, &SelectionOverlay::onToolChanged);
    connect(m_toolbar, &CaptureToolbar::styleChanged, this, [this] {
        if (m_textEdit) {
            m_textStyle = currentStyle();
            applyTextStyle();
        }
        if (m_selectedAnnotation >= 0 && m_annotations.at(m_selectedAnnotation)->spotlightRect().isEmpty()) {
            m_annotations.beginEdit();
            m_annotations.setItem(m_selectedAnnotation,
                                  m_annotations.at(m_selectedAnnotation)->restyled(currentStyle()));
            m_annotations.commitEdit();
            refreshUndoState();
            update();
        }
    });
    connect(m_toolbar, &CaptureToolbar::redactRequested, this, &SelectionOverlay::redactSensitive);
    connect(m_toolbar, &CaptureToolbar::undoRequested, this, &SelectionOverlay::undo);
    connect(m_toolbar, &CaptureToolbar::redoRequested, this, &SelectionOverlay::redo);
    connect(m_toolbar, &CaptureToolbar::cancelRequested, this, &SelectionOverlay::cancelled);
    connect(m_toolbar, &CaptureToolbar::actionRequested, this, &SelectionOverlay::trigger);
}

void SelectionOverlay::setPassive(bool passive)
{
    m_passive = passive;
    if (passive) {
        m_state = State::Hover;
        m_selection = QRect();
        m_hover = QRect();
        m_annotations.clear();
        m_toolbar->hide();
    } else if (m_cursorInside) {
        m_hover = regionAt(m_cursor);
    }
    update();
}

void SelectionOverlay::setAutoAction(std::optional<CaptureAction> action)
{
    m_autoAction = action;
    update();
}

void SelectionOverlay::preselect(const QRect &physicalRegion)
{
    const QRect region = physicalRegion & m_image.rect();
    if (region.isEmpty())
        return;
    m_selection = region;
    m_state = State::Selected;
    m_passive = false;
    updateToolbar();
    update();
}

void SelectionOverlay::editFixedRegion(const QRect &physicalRegion)
{
    m_fixedRegion = true;
    m_selection = physicalRegion & m_image.rect();
    m_state = State::Selected;
    m_toolbar->setPinEditing(true);
    updateToolbar();
    update();
}

CaptureAction SelectionOverlay::confirmAction() const
{
    if (m_fixedRegion)
        return CaptureAction::Pin;
    return m_autoAction.value_or(m_confirmAction);
}

CaptureAction SelectionOverlay::configuredConfirmAction()
{
    switch (Config::confirmAction()) {
    case Config::ConfirmAction::Save:
        return CaptureAction::Save;
    case Config::ConfirmAction::Pin:
        return CaptureAction::Pin;
    case Config::ConfirmAction::Copy:
        return CaptureAction::Copy;
    }
    return CaptureAction::Copy;
}

ScreenshotArea SelectionOverlay::captureArea() const
{
    return {currentRegion(), m_lasso};
}

QPoint SelectionOverlay::toPhysical(const QPointF &logical) const
{
    const QRect bounds = m_image.rect();
    return QPoint(qBound(bounds.left(), qFloor(logical.x() * m_dpr), bounds.right()),
                  qBound(bounds.top(), qFloor(logical.y() * m_dpr), bounds.bottom()));
}

QRectF SelectionOverlay::toLogical(const QRect &physical) const
{
    return QRectF(physical.x() / m_dpr, physical.y() / m_dpr, physical.width() / m_dpr, physical.height() / m_dpr);
}

QRect SelectionOverlay::regionAt(const QPoint &pos) const
{
    const QRect region = smallestRegionAt(m_windows, pos);
    return region.isEmpty() ? m_image.rect() : region;
}

QRect SelectionOverlay::currentRegion() const
{
    switch (m_state) {
    case State::Hover:
        return m_passive ? QRect() : m_hover;
    case State::Dragging:
        return m_dragMoved ? m_selection : m_hover;
    default:
        return m_selection;
    }
}

int SelectionOverlay::edgesAt(const QPointF &pos) const
{
    if (!m_lasso.isEmpty())
        return None; // a hand-drawn shape can be moved, not stretched
    const QRectF r = toLogical(m_selection);
    if (!r.adjusted(-kEdgeTolerance, -kEdgeTolerance, kEdgeTolerance, kEdgeTolerance).contains(pos))
        return None;

    int edges = None;
    if (qAbs(pos.x() - r.left()) <= kEdgeTolerance)
        edges |= Left;
    else if (qAbs(pos.x() - r.right()) <= kEdgeTolerance)
        edges |= Right;
    if (qAbs(pos.y() - r.top()) <= kEdgeTolerance)
        edges |= Top;
    else if (qAbs(pos.y() - r.bottom()) <= kEdgeTolerance)
        edges |= Bottom;
    return edges;
}

int SelectionOverlay::spotlightHandleAt(const QPointF &pos) const
{
    if (m_selectedAnnotation < 0)
        return None;
    const QRectF focus = m_annotations.at(m_selectedAnnotation)->spotlightRect();
    if (focus.isEmpty())
        return None;

    const auto points = handleCenters(toLogical(focus.toRect()));
    constexpr int edges[] = {Left | Top, Top, Right | Top, Right, Right | Bottom, Bottom, Left | Bottom, Left};
    constexpr qreal tolerance = kHandleSize / 2 + 2;
    for (size_t i = 0; i < points.size(); ++i) {
        const QPointF delta = pos - points[i];
        if (qAbs(delta.x()) <= tolerance && qAbs(delta.y()) <= tolerance)
            return edges[i];
    }
    return None;
}

Qt::CursorShape SelectionOverlay::resizeCursor(int edges)
{
    switch (edges) {
    case Left | Top:
    case Right | Bottom:
        return Qt::SizeFDiagCursor;
    case Right | Top:
    case Left | Bottom:
        return Qt::SizeBDiagCursor;
    case Left:
    case Right:
        return Qt::SizeHorCursor;
    case Top:
    case Bottom:
        return Qt::SizeVerCursor;
    default:
        return Qt::SizeAllCursor;
    }
}

void SelectionOverlay::moveSelectionTo(QRect rect)
{
    const QRect bounds = m_image.rect();
    rect.moveLeft(qBound(bounds.left(), rect.left(), bounds.right() - rect.width() + 1));
    rect.moveTop(qBound(bounds.top(), rect.top(), bounds.bottom() - rect.height() + 1));
    m_lasso.translate(rect.topLeft() - m_selection.topLeft()); // a freeform shape moves along
    m_selection = rect;
}

void SelectionOverlay::setFreeform(bool on)
{
    m_freeform = on;
    update();
}

void SelectionOverlay::resetToHover()
{
    m_state = State::Hover;
    m_selection = QRect();
    m_lasso = QPainterPath();
    m_lassoPoints.clear();
    m_hover = regionAt(m_cursor);
    selectAnnotation(-1);
    m_annotations.clear();
    m_draft.reset();
    refreshUndoState();
    m_toolbar->hide();
    setCursor(Qt::CrossCursor);
    emit selectionCleared();
    update();
}

void SelectionOverlay::updateCursor(const QPointF &pos)
{
    if (m_state != State::Selected) {
        setCursor(Qt::CrossCursor);
        return;
    }

    const QPoint physical = toPhysical(pos);
    if (m_toolbar->tool() == Tool::Eraser) {
        setCursor(m_selection.contains(physical) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        return;
    }
    if (const int edges = spotlightHandleAt(pos); edges != None) {
        setCursor(resizeCursor(edges));
        return;
    }
    if (m_selection.contains(physical) && m_annotations.itemAt(QPointF(physical), hitTolerance()) >= 0) {
        setCursor(Qt::SizeAllCursor); // an existing mark: drag to move it
        return;
    }

    const Tool tool = m_toolbar->tool();
    if (m_fixedRegion && tool == Tool::None) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (tool != Tool::None) {
        if (!m_selection.contains(toPhysical(pos)))
            setCursor(Qt::ArrowCursor);
        else
            setCursor(tool == Tool::Text ? Qt::IBeamCursor : Qt::CrossCursor);
        return;
    }

    const int edges = edgesAt(pos);
    setCursor(edges != None || m_selection.contains(physical) ? resizeCursor(edges) : Qt::CrossCursor);
}

void SelectionOverlay::updateToolbar()
{
    if ((m_state != State::Selected && m_state != State::Annotating) || m_passive) {
        m_toolbar->hide();
        return;
    }
    // The work area and the toolbar both use logical pixels on this overlay's screen.
    const QRect workArea = m_screen->availableGeometry();
    const QRect localWorkArea(mapFromGlobal(workArea.topLeft()), workArea.size());
    const QRect bounds = rect().intersected(localWorkArea);
    m_toolbar->setGeometry(toolbarGeometry(*m_toolbar, bounds.isEmpty() ? rect() : bounds,
                                           toLogical(m_selection).toAlignedRect()));
    m_toolbar->show();
    m_toolbar->raise();
}

void SelectionOverlay::trigger(CaptureAction action)
{
    commitText();
    const ScreenshotArea area = captureArea();
    const QRect &region = area.bounds;
    if (region.isEmpty())
        return;
    if (m_fixedRegion && action != CaptureAction::Pin)
        return; // editing a pin: only "done" applies
    if (isRegionAction(action)) {
        if (region.width() >= 16 && region.height() >= 16)
            emit regionActionTriggered(action, m_screen, region);
        return;
    }

    QPixmap image = QPixmap::fromImage(area.render(m_image, &m_annotations));
    if (image.isNull())
        return;
    image.setDevicePixelRatio(m_dpr);
    const QPoint globalTopLeft = m_screen->geometry().topLeft() + (QPointF(region.topLeft()) / m_dpr).toPoint();
    emit actionTriggered(action, image, globalTopLeft);
}

AnnotationStyle SelectionOverlay::currentStyle() const
{
    const int level = m_toolbar->sizeLevel();
    return {m_toolbar->color(), kStrokeWidths[level] * m_dpr, kFontSizes[level] * m_dpr, kMosaicCells[level] * m_dpr,
            m_toolbar->fill()};
}

void SelectionOverlay::onToolChanged()
{
    commitText();
    if (m_toolbar->tool() != Tool::None)
        selectAnnotation(-1);
    updateToolbar(); // the options row appears or disappears, changing the toolbar's size
    updateCursor(mapFromGlobal(QCursor::pos()));
}

void SelectionOverlay::updateDraft(Qt::KeyboardModifiers modifiers)
{
    const AnnotationStyle style = currentStyle();
    const Tool tool = m_toolbar->tool();
    QPoint to = m_cursor;

    switch (tool) {
    case Tool::Rect:
    case Tool::Ellipse:
        if (modifiers & Qt::ShiftModifier) { // square / circle
            const QPoint delta = to - m_pressPos;
            const int side = qMax(qAbs(delta.x()), qAbs(delta.y()));
            to = m_pressPos + QPoint(delta.x() < 0 ? -side : side, delta.y() < 0 ? -side : side);
        }
        m_draft = makeShape(tool, m_pressPos, to, style);
        break;
    case Tool::Line:
    case Tool::Arrow:
    case Tool::Ruler:
        if (modifiers & Qt::ShiftModifier) { // nearest multiple of 45 degrees
            QLineF line(m_pressPos, to);
            line.setAngle(std::round(line.angle() / 45) * 45);
            to = line.p2().toPoint();
        }
        m_draft = makeShape(tool, m_pressPos, to, style);
        break;
    case Tool::Pen:
    case Tool::Highlighter: {
        const QPolygonF points = (modifiers & Qt::ShiftModifier) ? QPolygonF{QPointF(m_pressPos), QPointF(to)}
                                                                 : QPolygonF(m_strokePoints);
        m_draft = makeStroke(points, style, tool == Tool::Highlighter);
        break;
    }
    case Tool::Mosaic:
    case Tool::Blur:
        m_draft = makeMosaic(QRect(m_pressPos, to).normalized() & m_selection, m_image, style, tool == Tool::Blur);
        break;
    case Tool::Spotlight:
        m_draft = makeSpotlight(QRectF(m_pressPos, to).normalized().intersected(QRectF(m_selection)));
        break;
    default:
        m_draft.reset();
    }
}

void SelectionOverlay::finishDraft()
{
    const Tool tool = m_toolbar->tool();
    const bool meaningful = (tool == Tool::Pen || tool == Tool::Highlighter)
                                ? m_strokePoints.size() > 1
                                : (m_cursor - m_pressPos).manhattanLength() >= kDragThreshold;
    if (m_draft && meaningful) {
        selectAnnotation(-1);
        m_annotations.add(m_draft);
    }
    m_draft.reset();
    m_strokePoints.clear();
    m_state = State::Selected;
    refreshUndoState();
    update();
}

void SelectionOverlay::updateAnnotationDrag()
{
    const QPointF delta(m_cursor - m_pressPos);
    const QRectF original = m_movingOriginal->spotlightRect();
    if (original.isEmpty()) {
        m_annotations.setItem(m_selectedAnnotation, m_movingOriginal->moved(delta));
        return;
    }

    const QRectF selection(m_selection);
    QRectF focus = original;
    if (m_resizeEdges == None) {
        const auto clampOffset = [](qreal offset, qreal first, qreal last) {
            return qBound(qMin(first, last), offset, qMax(first, last));
        };
        focus.translate(clampOffset(delta.x(), selection.left() - focus.left(), selection.right() - focus.right()),
                        clampOffset(delta.y(), selection.top() - focus.top(), selection.bottom() - focus.bottom()));
    } else {
        if (m_resizeEdges & Left)
            focus.setLeft(focus.left() + delta.x());
        if (m_resizeEdges & Right)
            focus.setRight(focus.right() + delta.x());
        if (m_resizeEdges & Top)
            focus.setTop(focus.top() + delta.y());
        if (m_resizeEdges & Bottom)
            focus.setBottom(focus.bottom() + delta.y());
        focus = focus.normalized().intersected(selection);
    }
    m_annotations.setItem(m_selectedAnnotation, focus == original ? m_movingOriginal : makeSpotlight(focus));
}

void SelectionOverlay::eraseAlong(const QPoint &from, const QPoint &to)
{
    const qreal step = hitTolerance();
    const int samples = qMax(1, qCeil(QLineF(from, to).length() / step));
    bool erased = false;
    for (int i = 0; i <= samples; ++i) {
        const QPointF pos = QPointF(from) + QPointF(to - from) * (qreal(i) / samples);
        for (int hit; (hit = m_annotations.itemAt(pos, hitTolerance())) >= 0; erased = true)
            m_annotations.remove(hit);
    }
    if (erased)
        update();
}

void SelectionOverlay::redactSensitive()
{
    commitText();
    const QRect region = currentRegion();
    if (region.isEmpty() || (m_redactWatcher && m_redactWatcher->isRunning()))
        return;
    if (!m_redactWatcher) {
        m_redactWatcher = new QFutureWatcher<OcrResult>(this);
        connect(m_redactWatcher, &QFutureWatcherBase::finished, this, [this] {
            unsetCursor();
            updateCursor(mapFromGlobal(QCursor::pos()));
            const OcrResult result = m_redactWatcher->result();
            if (!result.error.isEmpty()) {
                showNotice(tr("识别失败：%1").arg(result.error), 5000);
                emit redactFinished(-1);
                return;
            }
            const QList<QRect> boxes = Sensitive::regions(result.lines);
            selectAnnotation(-1);
            m_annotations.beginEdit(); // the whole redaction is one undo step
            for (const QRect &box : boxes) {
                AnnotationStyle style = currentStyle();
                style.mosaicCell = qMax(style.mosaicCell, box.height() / 3.0); // a few cells per line: unreadable
                m_annotations.add(makeMosaic(box.translated(m_redactRegion.topLeft()) & m_selection, m_image, style));
            }
            m_annotations.commitEdit();
            refreshUndoState();
            showNotice(boxes.isEmpty() ? tr("没有发现手机号、邮箱、身份证号、银行卡号或 IP 地址")
                                       : tr("已打码 %1 处 · Ctrl+Z 撤销").arg(boxes.size()));
            emit redactFinished(int(boxes.size()));
        });
        m_noticeTimer.setSingleShot(true);
        connect(&m_noticeTimer, &QTimer::timeout, this, [this] {
            m_notice.clear();
            update();
        });
    }
    m_redactRegion = region;
    setCursor(Qt::BusyCursor);
    showNotice(tr("正在识别敏感信息…"), 60'000);
    const QImage image = m_image.copy(region);
    const Config::OcrEngine engine = Config::ocrEngine();
    m_redactWatcher->setFuture(QtConcurrent::run([image, engine] { return Ocr::recognize(image, engine); }));
}

void SelectionOverlay::showNotice(const QString &text, int ms)
{
    m_notice = text;
    m_noticeTimer.start(ms);
    update();
}

void SelectionOverlay::undo()
{
    commitText();
    selectAnnotation(-1);
    m_annotations.undo();
    refreshUndoState();
    update();
}

void SelectionOverlay::redo()
{
    commitText();
    selectAnnotation(-1);
    m_annotations.redo();
    refreshUndoState();
    update();
}

void SelectionOverlay::refreshUndoState()
{
    m_toolbar->setUndoState(m_annotations.canUndo(), m_annotations.canRedo());
}

qreal SelectionOverlay::hitTolerance() const
{
    return 4 * m_dpr; // logical px of slack around a mark's stroke
}

void SelectionOverlay::selectAnnotation(int index)
{
    m_selectedAnnotation = index;
    const bool hasStyle = index >= 0 && m_annotations.at(index)->spotlightRect().isEmpty();
    m_toolbar->setSelectedAnnotationHasStyle(index >= 0 ? std::optional<bool>(hasStyle) : std::nullopt);
    if (hasStyle) {
        // The style row shows the mark's style, so currentStyle() reproduces it on edits.
        const AnnotationStyle style = m_annotations.at(index)->style();
        int level = 0;
        for (int i = 1; i < 3; ++i) {
            if (qAbs(style.width - kStrokeWidths[i] * m_dpr) < qAbs(style.width - kStrokeWidths[level] * m_dpr))
                level = i;
        }
        m_toolbar->showStyle(style.color, level, style.fill);
    }
    updateToolbar();
    update();
}

void SelectionOverlay::editTextAnnotation(int index)
{
    const AnnotationPtr item = m_annotations.at(index);
    selectAnnotation(-1);
    m_annotations.remove(index);
    refreshUndoState();
    beginText(item->anchor().toPoint(), item->text().value_or(QString()), item->style());
}

void SelectionOverlay::beginText(const QPoint &pos, const QString &text, std::optional<AnnotationStyle> style)
{
    m_textPos = pos;
    m_textStyle = style.value_or(currentStyle());

    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setFrameShape(QFrame::NoFrame);
    m_textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_textEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textEdit->document()->setDocumentMargin(0);
    m_textEdit->installEventFilter(this);
    connect(m_textEdit, &QPlainTextEdit::textChanged, this, &SelectionOverlay::layoutTextEdit);

    m_textEdit->setPlainText(text);
    m_textEdit->moveCursor(QTextCursor::End);
    applyTextStyle();
    m_textEdit->show();
    m_textEdit->setFocus();
}

void SelectionOverlay::applyTextStyle()
{
    m_textEdit->setFont(annotationFont(m_textStyle.fontSize / m_dpr));
    const QColor ink = m_textStyle.color.lightness() > 160 ? Qt::black : Qt::white;
    m_textEdit->setStyleSheet(QStringLiteral("QPlainTextEdit { background: %1; color: %2; "
                                             "border: 1px dashed %3; padding: 0; }")
                                  .arg(m_textStyle.fill ? m_textStyle.color.name() : QStringLiteral("transparent"),
                                       (m_textStyle.fill ? ink : m_textStyle.color).name(), kAccent.name()));
    layoutTextEdit();
}

void SelectionOverlay::layoutTextEdit()
{
    if (!m_textEdit)
        return;
    const QFontMetrics fm(m_textEdit->font());
    const QStringList lines = m_textEdit->toPlainText().split(QLatin1Char('\n'));
    int width = 0;
    for (const QString &line : lines)
        width = qMax(width, fm.horizontalAdvance(line));
    m_textEdit->resize(width + fm.averageCharWidth() * 2 + 8, int(lines.size()) * fm.lineSpacing() + 8);

    // Shift by the dashed border so typed text sits where the committed text will be drawn.
    const QPointF origin = QPointF(m_textPos) / m_dpr;
    m_textEdit->move(qRound(origin.x()) - 1, qRound(origin.y()) - 1);
}

void SelectionOverlay::commitText()
{
    if (!m_textEdit)
        return;
    QPlainTextEdit *edit = m_textEdit;
    m_textEdit = nullptr;
    const QString text = edit->toPlainText();
    edit->removeEventFilter(this);
    edit->deleteLater();

    if (!text.trimmed().isEmpty()) {
        selectAnnotation(-1);
        m_annotations.add(makeText(m_textPos, text, m_textStyle));
    }
    refreshUndoState();
    setFocus();
    update();
}

void SelectionOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.drawPixmap(0, 0, m_shot);

    const QRect region = currentRegion();
    const QRectF r = toLogical(region);
    QPainterPath dim;
    dim.addRect(rect());
    QPainterPath hole;
    if (!m_lasso.isEmpty())
        hole = QTransform::fromScale(1.0 / m_dpr, 1.0 / m_dpr).map(m_lasso);
    else if (!region.isEmpty())
        hole.addRect(r);
    if (!hole.isEmpty())
        dim = dim.subtracted(hole);
    p.fillPath(dim, kDim);
    if (m_state == State::Lassoing && m_lassoPoints.size() > 1) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(kAccent, 2));
        p.drawPolyline(QTransform::fromScale(1.0 / m_dpr, 1.0 / m_dpr).map(m_lassoPoints));
        p.restore();
    }

    if (!region.isEmpty()) {
        if (!m_annotations.isEmpty() || m_draft) {
            p.save();
            if (!hole.isEmpty())
                p.setClipPath(hole);
            else
                p.setClipRect(r);
            p.scale(1.0 / m_dpr, 1.0 / m_dpr); // annotations live in physical pixels
            m_annotations.paint(p, region, m_draft);
            p.restore();
        }
        if (m_selectedAnnotation >= 0) {
            const auto &selected = m_annotations.at(m_selectedAnnotation);
            const QRectF focus = selected->spotlightRect();
            p.setPen(QPen(kAccent, 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            if (focus.isEmpty()) {
                const QRectF box = selected->bounds();
                const QRectF logical(box.x() / m_dpr, box.y() / m_dpr, box.width() / m_dpr, box.height() / m_dpr);
                p.drawRect(logical.adjusted(-3, -3, 3, 3));
            } else {
                const QRectF logical = toLogical(focus.toRect());
                p.drawRect(logical);
                drawHandles(p, logical);
            }
        }

        p.setPen(QPen(kAccent, 2));
        p.setBrush(Qt::NoBrush);
        if (m_lasso.isEmpty()) {
            p.drawRect(r.adjusted(1, 1, -1, -1));
        } else {
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.drawPath(hole);
            p.restore();
        }
        const bool adjustable = m_state == State::Selected || m_state == State::Moving || m_state == State::Resizing;
        if (adjustable && !m_fixedRegion && m_lasso.isEmpty()) // a pinned image's region cannot be resized
            drawHandles(p, r);
        drawSizeLabel(p, region);
    }

    if (m_showMagnifier && m_cursorInside && picking() && !m_passive)
        drawMagnifier(p);
    if (m_autoAction && !m_passive)
        drawAutoActionBanner(p);
    else if (m_freeform && !m_passive && (m_state == State::Hover || m_state == State::Lassoing))
        drawBanner(p, tr("自由形状截图：按住鼠标画出范围  ·  F 切回矩形  ·  Esc 取消"));
    if (!m_notice.isEmpty())
        drawBanner(p, m_notice);
}

void SelectionOverlay::drawAutoActionBanner(QPainter &p) const
{
    QString text;
    switch (*m_autoAction) {
    case CaptureAction::LongShot:
        text = tr("框选要长截图的区域，松开后开始滚动拼接");
        break;
    case CaptureAction::RecordGif:
        text = tr("框选要录制的区域，松开后开始录制 GIF");
        break;
    case CaptureAction::RecordMp4:
        text = tr("框选要录制的区域，松开后开始录制 MP4");
        break;
    default:
        return;
    }
    text += tr("  ·  Esc 取消");
    drawBanner(p, text);
}

void SelectionOverlay::drawBanner(QPainter &p, const QString &text) const
{
    const QFontMetricsF fm(font());
    QRectF box(0, 0, fm.horizontalAdvance(text) + 32, fm.height() + 16);
    box.moveCenter(QPointF(width() / 2.0, 24 + box.height() / 2));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 190));
    p.drawRoundedRect(box, box.height() / 2, box.height() / 2);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, text);
    p.restore();
}

void SelectionOverlay::drawHandles(QPainter &p, const QRectF &r) const
{
    p.setPen(QPen(kAccent, 1));
    p.setBrush(Qt::white);
    for (const QPointF &pt : handleCenters(r))
        p.drawRect(QRectF(pt.x() - kHandleSize / 2, pt.y() - kHandleSize / 2, kHandleSize, kHandleSize));
}

void SelectionOverlay::drawSizeLabel(QPainter &p, const QRect &region) const
{
    const QString text = QStringLiteral("%1 × %2").arg(region.width()).arg(region.height());
    const QFontMetricsF fm(font());
    QRectF box(0, 0, fm.horizontalAdvance(text) + 12, fm.height() + 6);
    const QRectF r = toLogical(region);
    box.moveBottomLeft(QPointF(r.left(), r.top() - 4));
    if (box.top() < 0)
        box.moveTopLeft(QPointF(r.left() + 4, r.top() + 4));

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 170));
    p.drawRoundedRect(box, 3, 3);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, text);
    p.restore();
}

bool SelectionOverlay::picking() const
{
    return m_state == State::Hover || m_state == State::Dragging || m_state == State::Resizing;
}

QString SelectionOverlay::colorText(const QColor &color) const
{
    return m_rgbColors ? QStringLiteral("%1, %2, %3").arg(color.red()).arg(color.green()).arg(color.blue())
                       : color.name().toUpper();
}

void SelectionOverlay::selectTool(Tool tool)
{
    m_toolbar->setTool(m_toolbar->tool() == tool ? Tool::None : tool);
    onToolChanged();
}

void SelectionOverlay::drawMagnifier(QPainter &p) const
{
    constexpr int kSide = kMagnifierSource * kMagnifierZoom;
    constexpr int kInfoHeight = 58;
    constexpr int kOffset = 20;

    QImage zoom(kMagnifierSource, kMagnifierSource, QImage::Format_RGB32);
    zoom.fill(Qt::black);
    const QPoint origin = m_cursor - QPoint(kMagnifierSource / 2, kMagnifierSource / 2);
    for (int y = 0; y < kMagnifierSource; ++y) {
        for (int x = 0; x < kMagnifierSource; ++x) {
            const QPoint src = origin + QPoint(x, y);
            if (m_image.rect().contains(src))
                zoom.setPixel(x, y, m_image.pixel(src));
        }
    }

    const QPointF cursor = QPointF(m_cursor) / m_dpr;
    QRectF box(cursor + QPointF(kOffset, kOffset), QSizeF(kSide, kSide + kInfoHeight));
    if (box.right() > width())
        box.moveRight(cursor.x() - kOffset);
    if (box.bottom() > height())
        box.moveBottom(cursor.y() - kOffset);
    const QRectF zoomRect(box.topLeft(), QSizeF(kSide, kSide));
    const QRectF infoRect(zoomRect.bottomLeft(), QSizeF(kSide, kInfoHeight));

    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(zoomRect, zoom);

    // Crosshair bands through the center pixel.
    const qreal mid = (kMagnifierSource / 2) * kMagnifierZoom;
    const QColor band(30, 144, 255, 70);
    p.fillRect(QRectF(zoomRect.left(), zoomRect.top() + mid, kSide, kMagnifierZoom), band);
    p.fillRect(QRectF(zoomRect.left() + mid, zoomRect.top(), kMagnifierZoom, kSide), band);
    p.setPen(QPen(Qt::white, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(zoomRect.left() + mid, zoomRect.top() + mid, kMagnifierZoom, kMagnifierZoom));

    const QColor color = m_image.pixelColor(m_cursor);
    p.fillRect(infoRect, QColor(0, 0, 0, 200));
    p.fillRect(QRectF(infoRect.left() + 8, infoRect.top() + 22, 10, 10), color);
    p.setPen(Qt::white);
    p.drawText(infoRect.adjusted(8, 4, -4, 0), Qt::AlignLeft | Qt::AlignTop,
               QStringLiteral("(%1, %2)").arg(m_cursor.x()).arg(m_cursor.y()));
    p.drawText(infoRect.adjusted(24, 19, -4, 0), Qt::AlignLeft | Qt::AlignTop, colorText(color));
    p.setPen(QColor(255, 255, 255, 160));
    p.drawText(infoRect.adjusted(8, 36, -4, 0), Qt::AlignLeft | Qt::AlignTop, tr("C 复制 · Shift 切换格式"));
    p.setPen(QPen(QColor(255, 255, 255, 200), 1));
    p.drawRect(box);
    p.restore();
}

void SelectionOverlay::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = toPhysical(event->position());
    const Tool tool = m_toolbar->tool();

    if (event->button() == Qt::RightButton) {
        commitText();
        if (tool != Tool::None) {
            m_toolbar->setTool(Tool::None);
            onToolChanged();
        } else if (m_state == State::Selected && !m_fixedRegion) {
            resetToHover();
        } else {
            emit cancelled();
        }
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    if (m_textEdit) { // clicking anywhere outside the text box finishes it
        commitText();
        return;
    }

    if (m_state == State::Selected && tool == Tool::Eraser) {
        if (m_selection.contains(pos)) {
            m_annotations.beginEdit(); // everything erased in one drag is one undo step
            m_state = State::Erasing;
            m_pressPos = pos; // while erasing: where the eraser was last
            eraseAlong(pos, pos);
        }
        return;
    }

    // An existing mark under the cursor is selected and dragged, whatever tool is active.
    const int annotationEdges = m_state == State::Selected ? spotlightHandleAt(event->position()) : None;
    if (m_state == State::Selected && (annotationEdges != None || m_selection.contains(pos))) {
        const int hit = annotationEdges != None ? m_selectedAnnotation : m_annotations.itemAt(QPointF(pos), hitTolerance());
        if (hit >= 0) {
            selectAnnotation(hit);
            m_pressPos = pos;
            m_resizeEdges = annotationEdges;
            m_movingOriginal = m_annotations.at(hit);
            m_annotations.beginEdit();
            m_state = State::MovingAnnotation;
            return;
        }
        if (m_selectedAnnotation >= 0)
            selectAnnotation(-1);
    }

    if (m_state == State::Selected && tool != Tool::None) {
        if (!m_selection.contains(pos))
            return;
        m_pressPos = pos;
        m_cursor = pos;
        switch (tool) {
        case Tool::Text:
            beginText(pos);
            return;
        case Tool::Number:
            selectAnnotation(-1);
            m_annotations.add(makeNumber(pos, m_annotations.nextNumber(), currentStyle()));
            refreshUndoState();
            update();
            return;
        default:
            m_state = State::Annotating;
            m_strokePoints = {QPointF(pos)};
            updateDraft(event->modifiers());
            update();
            return;
        }
    }

    if (m_fixedRegion)
        return;
    m_passive = false;
    emit selectionStarted();
    m_pressPos = pos;
    m_pressSelection = m_selection;

    if (m_state == State::Selected) {
        m_resizeEdges = edgesAt(event->position());
        if (m_resizeEdges != None) {
            m_state = State::Resizing;
            m_toolbar->hide();
            return;
        }
        if (m_selection.contains(pos)) {
            m_state = State::Moving;
            m_toolbar->hide();
            return;
        }
        if (!m_annotations.isEmpty()) // starting over would silently drop the annotations
            return;
    }
    m_toolbar->hide();
    m_lasso = QPainterPath(); // a new region replaces the previous shape
    if (m_freeform) {
        m_state = State::Lassoing;
        m_lassoPoints = {QPointF(pos)};
        update();
        return;
    }
    m_state = State::Dragging;
    m_dragMoved = false;
    m_hover = regionAt(pos);
    update();
}

void SelectionOverlay::mouseMoveEvent(QMouseEvent *event)
{
    m_cursor = toPhysical(event->position());
    m_cursorInside = true;

    switch (m_state) {
    case State::Hover:
        if (!m_passive)
            m_hover = regionAt(m_cursor);
        break;
    case State::Dragging:
        if (!m_dragMoved && (m_cursor - m_pressPos).manhattanLength() >= kDragThreshold)
            m_dragMoved = true;
        if (m_dragMoved)
            m_selection = QRect(m_pressPos, m_cursor).normalized();
        break;
    case State::Moving:
        moveSelectionTo(m_pressSelection.translated(m_cursor - m_pressPos));
        break;
    case State::Resizing: {
        QRect r = m_pressSelection;
        const QPoint delta = m_cursor - m_pressPos;
        if (m_resizeEdges & Left)
            r.setLeft(r.left() + delta.x());
        if (m_resizeEdges & Right)
            r.setRight(r.right() + delta.x());
        if (m_resizeEdges & Top)
            r.setTop(r.top() + delta.y());
        if (m_resizeEdges & Bottom)
            r.setBottom(r.bottom() + delta.y());
        m_selection = r.normalized() & m_image.rect();
        break;
    }
    case State::MovingAnnotation:
        updateAnnotationDrag();
        break;
    case State::Lassoing:
        if (m_lassoPoints.last() != QPointF(m_cursor))
            m_lassoPoints.append(QPointF(m_cursor));
        break;
    case State::Erasing:
        eraseAlong(m_pressPos, m_cursor);
        m_pressPos = m_cursor;
        break;
    case State::Annotating:
        if (m_strokePoints.isEmpty() || m_strokePoints.last() != QPointF(m_cursor))
            m_strokePoints.append(QPointF(m_cursor));
        updateDraft(event->modifiers());
        break;
    case State::Selected:
        updateCursor(event->position());
        break;
    }
    update();
}

void SelectionOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;

    switch (m_state) {
    case State::Annotating:
        finishDraft();
        break;
    case State::Erasing:
        m_annotations.commitEdit();
        m_state = State::Selected;
        refreshUndoState();
        update();
        break;
    case State::Lassoing: {
        QPainterPath shape;
        shape.addPolygon(m_lassoPoints);
        shape.closeSubpath();
        m_lassoPoints.clear();
        const QRect bounds = shape.boundingRect().toAlignedRect() & m_image.rect();
        if (bounds.width() < 8 || bounds.height() < 8) { // a click, not a shape
            m_state = State::Hover;
            update();
            break;
        }
        m_lasso = shape;
        m_selection = bounds;
        m_state = State::Selected;
        if (m_autoAction) {
            trigger(*m_autoAction);
            return;
        }
        updateToolbar();
        updateCursor(event->position());
        update();
        break;
    }
    case State::MovingAnnotation:
        m_annotations.commitEdit(); // one undo step for the whole drag; none if it didn't move
        m_movingOriginal.reset();
        m_state = State::Selected;
        refreshUndoState();
        update();
        break;
    case State::Dragging:
        if (!m_dragMoved)
            m_selection = m_hover; // a plain click takes the highlighted window
        [[fallthrough]];
    case State::Moving:
    case State::Resizing:
        m_state = State::Selected;
        if (m_autoAction) {
            trigger(*m_autoAction);
            return;
        }
        updateToolbar();
        updateCursor(event->position());
        update();
        break;
    default:
        break;
    }
}

void SelectionOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_state == State::Selected && !m_textEdit) {
        const int hit = m_annotations.itemAt(QPointF(toPhysical(event->position())), hitTolerance());
        if (hit >= 0 && m_annotations.at(hit)->text()) {
            editTextAnnotation(hit);
            return;
        }
    }
    if (event->button() == Qt::LeftButton && m_state == State::Selected && m_toolbar->tool() == Tool::None
        && m_selection.contains(toPhysical(event->position())))
        trigger(confirmAction());
}

void SelectionOverlay::keyPressEvent(QKeyEvent *event)
{
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;

    // The arrow keys and W/A/S/D move the selection by 1 pixel, or 10 with Shift.
    QPoint nudge;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_A:
        nudge = {-1, 0};
        break;
    case Qt::Key_Right:
    case Qt::Key_D:
        nudge = {1, 0};
        break;
    case Qt::Key_Up:
    case Qt::Key_W:
        nudge = {0, -1};
        break;
    case Qt::Key_Down:
    case Qt::Key_S:
        nudge = {0, 1};
        break;
    default:
        break;
    }
    if (!nudge.isNull() && !ctrl) {
        if (m_state == State::Selected && !m_fixedRegion) {
            moveSelectionTo(m_selection.translated(nudge * (shift ? 10 : 1)));
            updateToolbar();
            update();
        }
        return;
    }

    switch (event->key()) {
    case Qt::Key_Escape:
        if (m_selectedAnnotation >= 0) // the first Esc only drops the mark selection
            selectAnnotation(-1);
        else
            emit cancelled();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (m_selectedAnnotation >= 0) {
            const int index = m_selectedAnnotation;
            selectAnnotation(-1);
            m_annotations.remove(index);
            refreshUndoState();
            update();
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        trigger(confirmAction());
        return;
    case Qt::Key_C:
        if (ctrl) {
            trigger(CaptureAction::Copy);
        } else if (m_cursorInside) {
            QGuiApplication::clipboard()->setText(colorText(m_image.pixelColor(m_cursor)));
            emit cancelled();
        }
        return;
    case Qt::Key_S:
        if (ctrl) {
            trigger(shift ? CaptureAction::QuickSave : CaptureAction::Save);
            return;
        }
        break;
    case Qt::Key_T:
        if (ctrl) {
            trigger(CaptureAction::Pin);
            return;
        }
        break;
    case Qt::Key_O:
        if (ctrl || (!shift && m_state == State::Selected)) {
            trigger(CaptureAction::Ocr);
            return;
        }
        break;
    case Qt::Key_F:
        if (!ctrl && m_state == State::Hover && !m_fixedRegion) {
            setFreeform(!m_freeform);
            return;
        }
        break;
    case Qt::Key_Shift:
        if (picking() && !event->isAutoRepeat()) { // holding Shift must not keep flipping
            m_rgbColors = !m_rgbColors;
            update();
            return;
        }
        break;
    case Qt::Key_L:
        if (ctrl) {
            trigger(CaptureAction::LongShot);
            return;
        }
        break;
    case Qt::Key_B:
        if (ctrl) {
            trigger(CaptureAction::Beautify);
            return;
        }
        break;
    case Qt::Key_D:
        if (ctrl && m_state == State::Selected) {
            redactSensitive();
            return;
        }
        break;
    case Qt::Key_R:
        if (ctrl) {
            trigger(shift ? CaptureAction::RecordMp4 : CaptureAction::RecordGif);
            return;
        }
        break;
    case Qt::Key_Z:
        if (ctrl) {
            shift ? redo() : undo();
            return;
        }
        break;
    case Qt::Key_Y:
        if (ctrl) {
            redo();
            return;
        }
        break;
    case Qt::Key_0:
    case Qt::Key_1:
    case Qt::Key_2:
    case Qt::Key_3:
    case Qt::Key_4:
    case Qt::Key_5:
    case Qt::Key_6:
    case Qt::Key_7:
    case Qt::Key_8:
    case Qt::Key_9:
        if (ctrl && m_state == State::Selected) {
            selectTool(kToolByDigit[event->key() - Qt::Key_0]);
            return;
        }
        break;
    default:
        break;
    }

    // Read the native virtual key when Qt reports a shifted symbol instead of a digit.
    if (shift && !ctrl && m_state == State::Selected) {
        int digit = -1;
        if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9)
            digit = event->key() - Qt::Key_0;
        else if (event->nativeVirtualKey() >= '1' && event->nativeVirtualKey() <= '9')
            digit = int(event->nativeVirtualKey()) - '0';
        if (digit > 0) {
            selectTool(kToolByShiftDigit[digit]);
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

bool SelectionOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_textEdit && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const bool ctrlEnter = (key->modifiers() & Qt::ControlModifier)
                               && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter);
        if (key->key() == Qt::Key_Escape || ctrlEnter) {
            commitText();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SelectionOverlay::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    const QPoint global = QCursor::pos();
    if (!m_screen->geometry().contains(global))
        return;
    m_cursor = toPhysical(QPointF(global - m_screen->geometry().topLeft()));
    m_cursorInside = true;
    m_hover = regionAt(m_cursor);
    activateWindow();
    setFocus();
}

void SelectionOverlay::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
    if (m_state == State::Hover) {
        activateWindow();
        setFocus();
    }
}

void SelectionOverlay::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_cursorInside = false;
    if (m_state == State::Hover)
        m_hover = QRect();
    update();
}
