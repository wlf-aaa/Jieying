#pragma once

#include <QDialog>
#include <QPixmap>
#include <QPointer>
#include <QStringList>

class CaptureHistory;
class HistoryPreview;
class QLabel;
class QListWidget;
class QPushButton;

class HistoryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HistoryDialog(CaptureHistory *history, QWidget *parent = nullptr);

signals:
    void pinRequested(const QPixmap &image);
    void collageRequested(const QStringList &paths);

private:
    QString currentPath() const;
    QStringList selectedPaths() const;
    void refreshEntries();
    void updateSelection();
    void updateCollageAction();
    QPixmap selectedImage();
    void copySelection();
    void saveSelection();
    void pinSelection();

    QPointer<CaptureHistory> m_history;
    QListWidget *m_list;
    HistoryPreview *m_preview;
    QLabel *m_dimensions;
    QPushButton *m_copy;
    QPushButton *m_saveAs;
    QPushButton *m_pin;
    QPushButton *m_collage;
};
