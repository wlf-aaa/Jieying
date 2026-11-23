#pragma once

#include "selectionoverlay.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <optional>

// One screenshot interaction: freezes every screen, lets the user pick a region on one of them,
// then hands the result on. Emits finished() exactly once.
class CaptureSession : public QObject
{
    Q_OBJECT

public:
    // With an autoAction, finishing a selection runs it straight away (hotkeys for long
    // screenshot / recording) instead of waiting at the toolbar.
    explicit CaptureSession(std::optional<CaptureAction> autoAction = std::nullopt, QObject *parent = nullptr);

    // Select this region (global physical pixels) right away, on the screen that contains it.
    void setPreselection(const QRect &globalPhysical) { m_preselection = globalPhysical; }
    // Regions are drawn by hand (see SelectionOverlay::setFreeform); also after start().
    void setFreeform(bool on);

    void start();

signals:
    void imageAction(CaptureAction action, const QPixmap &image, const QPoint &globalTopLeft,
                     QScreen *screen, const ScreenshotArea &area);
    void regionRequested(CaptureAction action, QScreen *screen, const QRect &physicalRegion);
    void finished();

private:
    void closeOverlays();
    void finish();

    std::optional<CaptureAction> m_autoAction;
    QRect m_preselection;
    QList<QPointer<SelectionOverlay>> m_overlays;
    bool m_finished = false;
};
