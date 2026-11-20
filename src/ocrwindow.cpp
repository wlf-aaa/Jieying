#include "ocrwindow.h"

#include "config.h"
#include "icons.h"
#include "translator.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextBlock>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <functional>

// Image scaled to fit, with each recognized line outlined; clicking a line reports its index.
class OcrPreview : public QWidget
{
public:
    explicit OcrPreview(const QImage &image, QWidget *parent = nullptr)
        : QWidget(parent), m_image(image)
    {
        setMinimumSize(200, 150);
        setMouseTracking(true);
    }

    void setLines(const QList<OcrLine> &lines)
    {
        m_lines = lines;
        update();
    }

    void setCodes(const QList<QPolygon> &codes)
    {
        m_codes = codes;
        update();
    }

    std::function<void(int)> onLineClicked;

    QSize sizeHint() const override { return QSize(480, 360); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(245, 246, 248));
        const QRectF target = imageRect();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, m_image);

        const qreal scale = target.width() / m_image.width();
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < m_lines.size(); ++i) {
            const QRectF box = mapBox(m_lines[i].box, target, scale);
            const bool hovered = i == m_hovered;
            p.setPen(QPen(QColor(30, 144, 255), hovered ? 2 : 1));
            p.setBrush(QColor(30, 144, 255, hovered ? 70 : 35));
            p.drawRect(box);
        }
        // QR codes / barcodes in green.
        p.setPen(QPen(QColor(24, 128, 56), 2));
        p.setBrush(QColor(24, 128, 56, 40));
        for (const QPolygon &code : std::as_const(m_codes)) {
            QPolygonF mapped;
            for (const QPoint &corner : code)
                mapped << QPointF(target.x() + corner.x() * scale, target.y() + corner.y() * scale);
            p.drawPolygon(mapped);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int line = lineAt(event->position());
        if (line != m_hovered) {
            m_hovered = line;
            setCursor(line >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int line = lineAt(event->position());
        if (line >= 0 && onLineClicked)
            onLineClicked(line);
    }

private:
    QRectF imageRect() const
    {
        const QSizeF available = QSizeF(size()) - QSizeF(16, 16);
        const QSizeF scaled = QSizeF(m_image.size()).scaled(available, Qt::KeepAspectRatio);
        const QSizeF fitted = scaled.width() > m_image.width() ? QSizeF(m_image.size()) : scaled; // never upscale
        return QRectF(QPointF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
    }

    static QRectF mapBox(const QRect &box, const QRectF &target, qreal scale)
    {
        return QRectF(target.x() + box.x() * scale, target.y() + box.y() * scale, box.width() * scale,
                      box.height() * scale)
            .adjusted(-2, -2, 2, 2);
    }

    int lineAt(const QPointF &pos) const
    {
        const QRectF target = imageRect();
        const qreal scale = target.width() / m_image.width();
        for (int i = 0; i < m_lines.size(); ++i) {
            if (mapBox(m_lines[i].box, target, scale).contains(pos))
                return i;
        }
        return -1;
    }

    QImage m_image;
    QList<OcrLine> m_lines;
    QList<QPolygon> m_codes;
    int m_hovered = -1;
};

OcrWindow::OcrWindow(const QImage &image, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::WindowStaysOnTopHint) // stays above pinned images
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("文字识别")); // Qt appends " - 截影" (the application display name)
    resize(960, 540);

    m_preview = new OcrPreview(image, this);
    m_preview->onLineClicked = [this](int line) { selectLine(line); };
    m_text = new QPlainTextEdit(this);
    m_text->setPlaceholderText(tr("正在识别…"));
    m_text->setReadOnly(true);
    QFont font = m_text->font();
    font.setPointSizeF(font.pointSizeF() * 1.15);
    m_text->setFont(font);

    // QR codes / barcodes found in the image, listed above the text (hidden when there are none).
    m_codes = new QWidget(this);
    m_codes->setLayout(new QVBoxLayout);
    m_codes->layout()->setContentsMargins(0, 0, 0, 4);
    m_codes->hide();
    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(m_codes);
    rightLayout->addWidget(m_text, 1);
    m_translation = new QPlainTextEdit(this);
    m_translation->setReadOnly(true);
    m_translation->setFont(font);
    m_translation->setPlaceholderText(tr("正在翻译…"));
    m_translation->hide();
    rightLayout->addWidget(m_translation, 1);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_preview);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_status = new QLabel(tr("正在识别…"), this);
    m_copy = new QPushButton(tr("复制全部"), this);
    m_copy->setEnabled(false);
    m_copy->setDefault(true);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_text->toPlainText());
        m_status->setText(tr("已复制到剪贴板"));
    });
    m_translate = new QPushButton(tr("翻译"), this);
    m_translate->setEnabled(false);
    m_translate->setToolTip(tr("用设置里选择的翻译服务翻译（需要联网）"));
    connect(m_translate, &QPushButton::clicked, this, &OcrWindow::translate);
    m_copyTranslation = new QPushButton(tr("复制译文"), this);
    m_copyTranslation->hide();
    connect(m_copyTranslation, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_translation->toPlainText());
        m_status->setText(tr("译文已复制到剪贴板"));
    });
    auto *close = new QPushButton(tr("关闭"), this);
    connect(close, &QPushButton::clicked, this, &QWidget::close);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_translate);
    buttons->addWidget(m_copyTranslation);
    buttons->addWidget(m_copy);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addLayout(buttons);

    connect(&m_watcher, &QFutureWatcher<OcrResult>::finished, this, [this] { showResult(m_watcher.result()); });
    m_watcher.setFuture(
        QtConcurrent::run([image, engine = Config::ocrEngine()] { return Ocr::recognize(image, engine); }));
#ifdef JIEYING_HAVE_ZXING
    connect(&m_codeWatcher, &QFutureWatcher<QList<Barcode>>::finished, this,
            [this] { showCodes(m_codeWatcher.result()); });
    m_codeWatcher.setFuture(QtConcurrent::run([image] { return Barcodes::detect(image); }));
#endif
}

QString OcrWindow::translation() const
{
    return m_translation->toPlainText();
}

void OcrWindow::translate()
{
    if (!Translator::isConfigured()) {
        m_status->setText(tr("请先在 托盘菜单 → 设置 → 翻译 里选择服务并填写密钥。"));
        return;
    }
    if (!Config::translateConsented()) {
        const auto answer = QMessageBox::question(
            this, tr("翻译"),
            tr("翻译会把识别出的文字发送给「%1」。\n截图里有隐私内容时请不要翻译。\n\n继续吗？（以后不再询问）")
                .arg(Translator::providerName(Config::translateProvider())));
        if (answer != QMessageBox::Yes)
            return;
        Config::setTranslateConsented(true);
    }
    if (!m_translator) {
        m_translator = new Translator(this);
        connect(m_translator, &Translator::finished, this, [this](const QString &text, const QString &error) {
            m_translate->setEnabled(true);
            m_translation->setPlainText(text);
            m_copyTranslation->setVisible(!text.isEmpty());
            m_status->setText(error.isEmpty() ? tr("翻译完成（%1）").arg(Translator::providerName(Config::translateProvider()))
                                              : error);
            emit translated();
        });
    }
    m_translate->setEnabled(false);
    m_translation->clear();
    m_translation->show();
    m_status->setText(tr("正在翻译…"));
    m_translator->translate(m_text->toPlainText());
}

QStringList OcrWindow::codes() const
{
    return m_codeTexts;
}

void OcrWindow::showCodes(const QList<Barcode> &codes)
{
    QList<QPolygon> outlines;
    for (const Barcode &code : codes) {
        outlines << code.corners;
        m_codeTexts << code.text;

        auto *row = new QWidget(m_codes);
        row->setObjectName("code");
        row->setAttribute(Qt::WA_StyledBackground);
        row->setStyleSheet("#code { background: #e8f5e9; border: 1px solid #a5d6a7; border-radius: 4px; }");
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 4, 4, 4);
        auto *label = new QLabel(QStringLiteral("<b>%1</b>　%2")
                                     .arg(code.format.contains(QLatin1String("QR")) ? tr("二维码") : code.format,
                                          code.text.toHtmlEscaped()),
                                 row);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        layout->addWidget(label, 1);
        auto *copy = new QPushButton(toolIcon("copy"), tr("复制"), row);
        connect(copy, &QPushButton::clicked, this, [this, text = code.text] {
            QApplication::clipboard()->setText(text);
            m_status->setText(tr("已复制二维码内容"));
        });
        layout->addWidget(copy);
        const QUrl url(code.text, QUrl::StrictMode);
        if (url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) {
            auto *open = new QPushButton(tr("打开链接"), row);
            connect(open, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(url); });
            layout->addWidget(open);
        }
        m_codes->layout()->addWidget(row);
    }
    m_codes->setVisible(!codes.isEmpty());
    m_preview->setCodes(outlines);
    m_codeAreas = outlines;
    // OCR may have finished first and read the code's finder squares as "回"; redo the text
    // unless the user already started editing it.
    if (m_ocrResult && !m_text->document()->isModified())
        displayText();
    emit codesDetected();
}

QString OcrWindow::text() const
{
    return m_text->toPlainText();
}

void OcrWindow::showResult(const OcrResult &result)
{
    m_ocrResult = result;
    displayText();
    emit recognized();
}

void OcrWindow::displayText()
{
    // Text lines inside a QR code / barcode are its modules misread as characters: drop them.
    OcrResult result = *m_ocrResult;
    result.lines.removeIf([this](const OcrLine &line) {
        return std::any_of(m_codeAreas.cbegin(), m_codeAreas.cend(), [&line](const QPolygon &code) {
            return code.boundingRect().adjusted(-4, -4, 4, 4).contains(line.box.center());
        });
    });

    m_text->setReadOnly(false); // allow fixing recognition mistakes before copying
    if (!result.error.isEmpty()) {
        m_status->setText(tr("识别失败：%1").arg(result.error));
        m_status->setStyleSheet("color: #d93025;");
    } else if (result.lines.isEmpty()) {
        m_text->setPlaceholderText(tr("没有识别到文字"));
        m_status->setText(tr("没有识别到文字"));
    } else {
        m_text->setPlainText(result.text());
        m_preview->setLines(result.lines);
        m_copy->setEnabled(true);
        m_translate->setEnabled(true);
        m_copy->setFocus();
        QString status = tr("识别到 %1 行 · %2 · 点击左侧文字框可定位到对应行").arg(result.lines.size()).arg(result.language);
        if (Config::ocrAutoCopy()) {
            QApplication::clipboard()->setText(result.text());
            status = tr("已复制到剪贴板 · ") + status;
        }
        m_status->setText(status);
    }
}

void OcrWindow::selectLine(int index)
{
    const QTextBlock block = m_text->document()->findBlockByNumber(index);
    if (!block.isValid())
        return;
    QTextCursor cursor(block);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    m_text->setTextCursor(cursor);
    m_text->setFocus();
}

void OcrWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
        close();
    else
        QWidget::keyPressEvent(event);
}
