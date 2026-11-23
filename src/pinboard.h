#pragma once

#include "pinwindow.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <optional>

class QMenu;

// All pinned images, in numbered groups. Only the current group is on screen; switching
// groups hides one set and brings back another, so different tasks keep their own pins.
class PinBoard : public QObject
{
    Q_OBJECT

public:
    explicit PinBoard(QObject *parent = nullptr);

    // Joins the current group and shows.
    void add(PinWindow *pin);
    QList<PinWindow *> pins() const;

    int currentGroup() const { return m_current; }
    QList<int> groups() const; // groups that have pins, ascending
    int count(int group) const;

    void showGroup(int group);
    // Next group with pins, wrapping; past the last one comes a fresh empty group.
    void nextGroup();
    void moveToGroup(PinWindow *pin, int group);

    // Hides the current group's pins, or shows them when all are hidden.
    void toggleCurrent();
    void closeAll();
    // Reopens the most recently closed pin once, in its original group.
    bool canRestoreLastClosed() const { return m_lastClosed.has_value(); }
    PinWindow *restoreLastClosed();
    bool anyClickThrough() const;
    void restoreClicks();

    // Fills a menu with the groups (checked: current) and "next group".
    void fillMenu(QMenu *menu);

signals:
    // After a switch, for a short on-screen note.
    void groupShown(int group, int pins);

private:
    void registerPin(PinWindow *pin);
    void addMoveMenu(PinWindow *pin, QMenu *menu);

    QList<QPointer<PinWindow>> m_pins;
    std::optional<PinWindow::Snapshot> m_lastClosed;
    int m_current = 1;
};
