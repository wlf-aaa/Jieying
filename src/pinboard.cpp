#include "pinboard.h"

#include <QGuiApplication>
#include <QMenu>
#include <QScreen>

#include <algorithm>

PinBoard::PinBoard(QObject *parent)
    : QObject(parent)
{
}

void PinBoard::registerPin(PinWindow *pin)
{
    m_pins.removeAll(nullptr);
    m_pins.append(pin);
    connect(pin, &PinWindow::menuAboutToShow, this, [this, pin](QMenu *menu) { addMoveMenu(pin, menu); });
    connect(pin, &PinWindow::closing, this, [this, pin] {
        // QWidget deletes the closed window later. Stop tracking it before a group can show it again.
        if (m_pins.removeAll(pin) && !pin->image().isNull())
            m_lastClosed = pin->snapshot();
    });
}

void PinBoard::add(PinWindow *pin)
{
    pin->setGroup(m_current);
    registerPin(pin);
    pin->show();
}

QList<PinWindow *> PinBoard::pins() const
{
    QList<PinWindow *> list;
    for (const auto &pin : m_pins) {
        if (pin)
            list.append(pin);
    }
    return list;
}

QList<int> PinBoard::groups() const
{
    QList<int> list;
    for (PinWindow *pin : pins()) {
        if (!list.contains(pin->group()))
            list.append(pin->group());
    }
    std::sort(list.begin(), list.end());
    return list;
}

int PinBoard::count(int group) const
{
    const QList<PinWindow *> all = pins();
    return int(std::count_if(all.cbegin(), all.cend(), [group](PinWindow *pin) { return pin->group() == group; }));
}

void PinBoard::showGroup(int group)
{
    m_current = qMax(1, group);
    for (PinWindow *pin : pins())
        pin->setVisible(pin->group() == m_current);
    emit groupShown(m_current, count(m_current));
}

void PinBoard::nextGroup()
{
    QList<int> candidates = groups();
    if (!candidates.contains(m_current))
        candidates.append(m_current);
    std::sort(candidates.begin(), candidates.end());
    if (count(candidates.last()) > 0)
        candidates.append(candidates.last() + 1); // a fresh, empty group to start over in
    const qsizetype at = candidates.indexOf(m_current);
    showGroup(candidates[(at + 1) % candidates.size()]);
}

void PinBoard::moveToGroup(PinWindow *pin, int group)
{
    pin->setGroup(group);
    pin->setVisible(group == m_current);
}

void PinBoard::toggleCurrent()
{
    QList<PinWindow *> current;
    for (PinWindow *pin : pins()) {
        if (pin->group() == m_current)
            current.append(pin);
    }
    const bool anyVisible = std::any_of(current.cbegin(), current.cend(), [](PinWindow *pin) { return pin->isVisible(); });
    for (PinWindow *pin : current)
        pin->setVisible(!anyVisible);
}

void PinBoard::closeAll()
{
    for (PinWindow *pin : pins())
        pin->close();
    m_pins.clear();
    m_current = 1;
}

PinWindow *PinBoard::restoreLastClosed()
{
    if (!m_lastClosed)
        return nullptr;

    auto *pin = new PinWindow(*m_lastClosed);
    m_lastClosed.reset();

    const QRect content = pin->contentGeometry();
    const QList<QScreen *> screens = QGuiApplication::screens();
    const bool onScreen = std::any_of(screens.cbegin(), screens.cend(), [content](const QScreen *screen) {
        return screen->availableGeometry().intersects(content);
    });
    if (QScreen *primary = QGuiApplication::primaryScreen(); primary && !onScreen)
        pin->move(pin->pos() + primary->availableGeometry().center() - content.center());

    registerPin(pin);
    if (pin->group() == m_current)
        pin->show();
    else
        showGroup(pin->group());
    return pin;
}

bool PinBoard::anyClickThrough() const
{
    const QList<PinWindow *> all = pins();
    return std::any_of(all.cbegin(), all.cend(), [](PinWindow *pin) { return pin->clickThrough(); });
}

void PinBoard::restoreClicks()
{
    for (PinWindow *pin : pins())
        pin->setClickThrough(false);
}

void PinBoard::fillMenu(QMenu *menu)
{
    menu->clear();
    QList<int> list = groups();
    if (!list.contains(m_current))
        list.append(m_current);
    std::sort(list.begin(), list.end());
    for (int group : list) {
        QAction *action = menu->addAction(tr("分组 %1（%2 张）").arg(group).arg(count(group)), this,
                                          [this, group] { showGroup(group); });
        action->setCheckable(true);
        action->setChecked(group == m_current);
    }
    menu->addSeparator();
    menu->addAction(tr("新建空分组"), this, [this] {
        const QList<int> used = groups();
        showGroup(qMax(m_current, used.isEmpty() ? 0 : used.last()) + 1);
    });
}

void PinBoard::addMoveMenu(PinWindow *pin, QMenu *menu)
{
    QMenu *move = menu->addMenu(tr("移到分组"));
    QList<int> list = groups();
    if (!list.contains(m_current))
        list.append(m_current);
    std::sort(list.begin(), list.end());
    for (int group : list) {
        QAction *action = move->addAction(tr("分组 %1").arg(group), this, [this, pin, group] { moveToGroup(pin, group); });
        action->setEnabled(group != pin->group());
    }
    const int fresh = list.last() + 1;
    move->addAction(tr("新分组 %1").arg(fresh), this, [this, pin, fresh] { moveToGroup(pin, fresh); });
}
