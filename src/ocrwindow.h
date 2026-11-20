#pragma once

#include "barcodes.h"
#include "ocr.h"

#include <QFutureWatcher>
#include <QImage>
#include <QWidget>

#include <optional>

class OcrPreview;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class Translator;

// Shows an image next to its recognized text. Recognition runs in the background
// as soon as the window is created.
class OcrWindow : public QWidget
{
    Q_OBJECT

public:
    explicit OcrWindow(const QImage &image, QWidget *parent = nullptr);

    QString text() const;
    QString translation() const;
    QStringList codes() const; // QR code / barcode contents found so far

signals:
    void recognized();
    void codesDetected();
    void translated();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void showResult(const OcrResult &result);
    void displayText();
    void showCodes(const QList<Barcode> &codes);
    void selectLine(int index);
    void translate();

    OcrPreview *m_preview;
    QPlainTextEdit *m_text;
    QLabel *m_status;
    QPushButton *m_copy;
    QPushButton *m_translate;
    QPushButton *m_copyTranslation;
    QPlainTextEdit *m_translation;
    Translator *m_translator = nullptr;
    QFutureWatcher<OcrResult> m_watcher;
    QFutureWatcher<QList<Barcode>> m_codeWatcher;
    QWidget *m_codes;
    QStringList m_codeTexts;
    QList<QPolygon> m_codeAreas;
    std::optional<OcrResult> m_ocrResult;
};
