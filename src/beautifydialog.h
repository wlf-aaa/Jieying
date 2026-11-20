#pragma once

#include "beautify.h"
#include "capturetoolbar.h"

#include <QImage>
#include <QPixmap>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

// Live preview of Beautify::render() with its options; Copy / Save / Pin hand the result on.
class BeautifyDialog : public QWidget
{
    Q_OBJECT

public:
    explicit BeautifyDialog(const QPixmap &image, QWidget *parent = nullptr);

    QImage result() const { return m_result; }
    // Programmatic option change (tests); same effect as the controls.
    void setOptions(const Beautify::Options &options);

signals:
    void actionRequested(CaptureAction action, const QPixmap &image, const QPoint &globalTopLeft);

private:
    void refresh();
    void finish(CaptureAction action);

    QImage m_source;
    Beautify::Options m_options;
    QImage m_result;
    QLabel *m_preview = nullptr;
    QComboBox *m_background = nullptr;
    QSlider *m_padding = nullptr;
    QSlider *m_radius = nullptr;
    QCheckBox *m_shadow = nullptr;
};
