#pragma once

#include "config.h"

#include <QDialog>
#include <QHash>

class HotkeyManager;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QListWidget;
class QStackedWidget;

// Settings window. Every change is applied and saved immediately; there is no OK/Cancel.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(HotkeyManager *hotkeys, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    // Page titles in sidebar order; showPage() selects one by index.
    QStringList pageTitles() const;
    void showPage(int index);
    // Same as picking the scheme in the hotkey page (used by tests).
    void applyHotkeyScheme(Config::HotkeyScheme scheme);
    QString schemeShown() const;

signals:
    void fullscreenHotkeyPauseEnabled(bool enabled);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void addPage(const QString &title, QWidget *page);
    QWidget *buildGeneralPage();
    QWidget *buildHotkeyPage();
    QWidget *buildSavePage();
    QWidget *buildAnnotationPage();
    QWidget *buildOcrPage();
    QWidget *buildTranslatePage();
    QWidget *buildRecordingPage();
    void showHotkeyStatus(Config::Hotkey hotkey, const QString &error, bool justApplied);
    void showCurrentScheme();
    void refreshHotkeys();

    HotkeyManager *m_hotkeys;
    QListWidget *m_sidebar;
    QStackedWidget *m_pages;
    QHash<Config::Hotkey, QLabel *> m_hotkeyStatus;
    QHash<Config::Hotkey, QKeySequenceEdit *> m_hotkeyEdits;
    QComboBox *m_scheme = nullptr;
};
