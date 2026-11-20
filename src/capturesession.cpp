#include "capturesession.h"

#include "windowfinder.h"

#include <QGuiApplication>
#include <QScreen>

CaptureSession::CaptureSession(std::optional<CaptureAction> autoAction, QObject *parent)
    : QObject(parent)
    , m_autoAction(autoAction)
{
}

void CaptureSession::setFreeform(bool on)
{
    for (const auto &overlay : std::as_const(m_overlays)) {
        if (overlay)
            overlay->setFreeform(on);
    }
}

void CaptureSession::start()
{
    // Snapshot windows and pixels for every screen before any overlay can cover them.
    const QList<WindowNode> windows = snapshotWindows();
    QList<std::pair<QScreen *, QPixmap>> shots;
    for (QScreen *screen : QGuiApplication::screens())
        shots.append({screen, screen->grabWindow(0)});

    for (const auto &[screen, shot] : shots) {
        auto *overlay = new SelectionOverlay(screen, shot, windows);
        overlay->setAutoAction(m_autoAction);
        connect(overlay, &SelectionOverlay::selectionStarted, this, [this, overlay] {
            for (const auto &other : std::as_const(m_overlays)) {
                if (other && other != overlay)
                    other->setPassive(true);
            }
        });
        connect(overlay, &SelectionOverlay::selectionCleared, this, [this] {
            for (const auto &other : std::as_const(m_overlays)) {
                if (other)
                    other->setPassive(false);
            }
        });
        connect(overlay, &SelectionOverlay::cancelled, this, &CaptureSession::finish);
        // Overlays are topmost, so they go before any dialog, pin or capture panel appears.
        connect(overlay, &SelectionOverlay::actionTriggered, this,
                [this](CaptureAction action, const QPixmap &image, const QPoint &globalTopLeft) {
                    closeOverlays();
                    emit imageAction(action, image, globalTopLeft);
                    finish();
                });
        connect(overlay, &SelectionOverlay::regionActionTriggered, this,
                [this](CaptureAction action, QScreen *screen, const QRect &region) {
                    closeOverlays();
                    emit regionRequested(action, screen, region);
                    finish();
                });
        m_overlays.append(overlay);
    }
    for (const auto &overlay : std::as_const(m_overlays))
        overlay->show();

    if (!m_preselection.isEmpty()) {
        // A screen's logical top-left equals its native one, which anchors its physical pixels.
        for (int i = 0; i < m_overlays.size(); ++i) {
            QScreen *screen = shots[i].first;
            const QRect physical(screen->geometry().topLeft(), screen->geometry().size() * screen->devicePixelRatio());
            if (physical.contains(m_preselection.center())) {
                m_overlays[i]->preselect(m_preselection.translated(-physical.topLeft()));
                m_overlays[i]->activateWindow();
                for (int j = 0; j < m_overlays.size(); ++j) {
                    if (j != i)
                        m_overlays[j]->setPassive(true);
                }
                break;
            }
        }
    }
}

void CaptureSession::closeOverlays()
{
    for (const auto &overlay : std::as_const(m_overlays)) {
        if (overlay)
            overlay->close();
    }
    m_overlays.clear();
}

void CaptureSession::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    closeOverlays();
    emit finished();
}
