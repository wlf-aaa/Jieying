#pragma once

#include "imagecollage.h"

#include <QDialog>
#include <QStringList>

class CollagePreview;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

class CollageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CollageDialog(QWidget *parent = nullptr);
    void addImages(const QStringList &paths);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void chooseImages();
    void removeImage();
    void moveImage(int step);
    void updateOrderButtons();
    void updatePreview();
    void exportPng();
    ImageCollage::Direction direction() const;

    QList<QImage> m_images;
    QListWidget *m_list = nullptr;
    CollagePreview *m_preview = nullptr;
    QComboBox *m_direction = nullptr;
    QSpinBox *m_spacing = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_moveUp = nullptr;
    QPushButton *m_moveDown = nullptr;
    QPushButton *m_export = nullptr;
};
