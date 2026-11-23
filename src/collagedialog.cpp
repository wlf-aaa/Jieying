#include "collagedialog.h"

#include "config.h"

#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace {

constexpr QSize kPreviewBounds(1600, 1000);

QStringList localPaths(const QMimeData *data)
{
    QStringList paths;
    for (const QUrl &url : data->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    return paths;
}

qint64 pixelCount(const QSize &size)
{
    return qint64(size.width()) * size.height();
}

bool fitsInputBudget(const QSize &size, qint64 remainingPixels)
{
    return size.width() <= ImageCollage::kMaxDimension && size.height() <= ImageCollage::kMaxDimension
           && pixelCount(size) <= remainingPixels;
}

} // namespace

class CollagePreview : public QWidget
{
public:
    explicit CollagePreview(QWidget *parent)
        : QWidget(parent)
    {
        setMinimumSize(300, 220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setImage(QImage image, const QString &message)
    {
        m_image = std::move(image);
        m_message = message;
        setAccessibleDescription(m_image.isNull() ? message : QString());
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().base());
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));

        const QRect display = rect().adjusted(10, 10, -10, -10);
        if (m_image.isNull()) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(display, Qt::AlignCenter | Qt::TextWordWrap, m_message);
            return;
        }

        const QSize size = m_image.size().scaled(display.size(), Qt::KeepAspectRatio)
                               .expandedTo(QSize(1, 1)).boundedTo(m_image.size());
        QRect target(QPoint(), size);
        target.moveCenter(display.center());
        painter.save();
        painter.setClipRect(target);
        constexpr int tile = 8;
        const QColor light(249, 249, 249);
        const QColor dark(223, 223, 223);
        for (int y = 0; y < size.height(); y += tile) {
            for (int x = 0; x < size.width(); x += tile) {
                const bool alternate = ((x + y) / tile) % 2 != 0;
                painter.fillRect(target.x() + x, target.y() + y, tile, tile, alternate ? dark : light);
            }
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, m_image);
        painter.restore();
    }

private:
    QImage m_image;
    QString m_message;
};

CollageDialog::CollageDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("多图拼排"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    resize(860, 520);

    auto *add = new QPushButton(tr("添加图片…"), this);
    m_remove = new QPushButton(tr("移除"), this);
    m_moveUp = new QPushButton(tr("上移"), this);
    m_moveDown = new QPushButton(tr("下移"), this);
    m_export = new QPushButton(tr("导出 PNG…"), this);
    for (QPushButton *button : {add, m_remove, m_moveUp, m_moveDown, m_export})
        button->setAutoDefault(false);

    m_list = new QListWidget(this);
    m_list->setAccessibleName(tr("图片顺序"));
    m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setDragDropMode(QAbstractItemView::NoDragDrop);
    m_list->setUniformItemSizes(true);

    auto *order = new QHBoxLayout;
    order->addWidget(m_remove);
    order->addWidget(m_moveUp);
    order->addWidget(m_moveDown);
    auto *inputs = new QVBoxLayout;
    inputs->addWidget(add);
    inputs->addWidget(m_list, 1);
    inputs->addLayout(order);

    m_direction = new QComboBox(this);
    m_direction->addItems({tr("横向"), tr("纵向")});
    auto *directionLabel = new QLabel(tr("方向"), this);
    directionLabel->setBuddy(m_direction);
    m_spacing = new QSpinBox(this);
    m_spacing->setRange(0, ImageCollage::kMaxDimension);
    m_spacing->setSuffix(tr(" 像素"));
    m_spacing->setKeyboardTracking(false);
    auto *spacingLabel = new QLabel(tr("间距"), this);
    spacingLabel->setBuddy(m_spacing);
    auto *options = new QHBoxLayout;
    options->addWidget(directionLabel);
    options->addWidget(m_direction);
    options->addSpacing(10);
    options->addWidget(spacingLabel);
    options->addWidget(m_spacing);
    options->addStretch();

    m_preview = new CollagePreview(this);
    m_preview->setAccessibleName(tr("拼接预览"));
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    auto *result = new QVBoxLayout;
    result->addLayout(options);
    result->addWidget(m_preview, 1);
    result->addWidget(m_summary);

    auto *body = new QHBoxLayout;
    body->addLayout(inputs, 1);
    body->addLayout(result, 2);
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_status, 1);
    actions->addWidget(m_export);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addLayout(actions);

    connect(add, &QPushButton::clicked, this, &CollageDialog::chooseImages);
    connect(m_remove, &QPushButton::clicked, this, &CollageDialog::removeImage);
    connect(m_moveUp, &QPushButton::clicked, this, [this] { moveImage(-1); });
    connect(m_moveDown, &QPushButton::clicked, this, [this] { moveImage(1); });
    connect(m_list, &QListWidget::currentRowChanged, this, &CollageDialog::updateOrderButtons);
    connect(m_direction, &QComboBox::currentIndexChanged, this, &CollageDialog::updatePreview);
    connect(m_spacing, &QSpinBox::valueChanged, this, &CollageDialog::updatePreview);
    connect(m_export, &QPushButton::clicked, this, &CollageDialog::exportPng);

    updateOrderButtons();
    updatePreview();
}

void CollageDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->possibleActions().testFlag(Qt::CopyAction) && !localPaths(event->mimeData()).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

void CollageDialog::dropEvent(QDropEvent *event)
{
    const QStringList paths = localPaths(event->mimeData());
    if (paths.isEmpty() || !event->possibleActions().testFlag(Qt::CopyAction))
        return;
    event->setDropAction(Qt::CopyAction);
    event->accept();
    addImages(paths);
}

void CollageDialog::chooseImages()
{
    QStringList patterns;
    for (const QByteArray &format : QImageReader::supportedImageFormats())
        patterns.append(QStringLiteral("*.") + QString::fromLatin1(format));
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("选择图片"), Config::effectiveSaveDir(),
                                                          tr("图片文件 (%1);;所有文件 (*)").arg(patterns.join(' ')));
    if (!paths.isEmpty())
        addImages(paths);
}

void CollageDialog::addImages(const QStringList &paths)
{
    qint64 remainingPixels = ImageCollage::kMaxPixels;
    for (const QImage &image : m_images)
        remainingPixels -= pixelCount(image.size());

    const int firstNewRow = m_list->count();
    int skipped = 0;
    QString firstError;
    const QString sizeError = tr("图片尺寸或已添加图片的总像素超过上限");
    for (const QString &path : paths) {
        const QFileInfo file(path);
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QImage image;
        QString reason;
        if (file.isFile() && reader.canRead()) {
            const QSize size = reader.size();
            if (size.isValid() && !fitsInputBudget(size, remainingPixels))
                reason = sizeError;
            else
                image = reader.read();
        }
        if (!image.isNull() && !fitsInputBudget(image.size(), remainingPixels))
            reason = sizeError;
        if (image.isNull() || !reason.isEmpty()) {
            if (!skipped) {
                if (reason.isEmpty())
                    reason = tr("无法读取图片");
                firstError = tr("%1（%2）").arg(file.fileName(), reason);
            }
            ++skipped;
            continue;
        }

        remainingPixels -= pixelCount(image.size());
        const QString dimensions = tr("%1 × %2").arg(image.width()).arg(image.height());
        m_images.append(std::move(image));
        auto *item = new QListWidgetItem(file.fileName(), m_list);
        item->setToolTip(path + QLatin1Char('\n') + dimensions);
    }

    if (m_list->count() > firstNewRow)
        m_list->setCurrentRow(firstNewRow);
    m_status->setText(skipped ? tr("已跳过 %1 项。%2").arg(skipped).arg(firstError) : QString());
    m_status->setToolTip(QString());
    updateOrderButtons();
    updatePreview();
}

void CollageDialog::removeImage()
{
    const int row = m_list->currentRow();
    if (row < 0)
        return;
    m_images.removeAt(row);
    delete m_list->takeItem(row);
    m_list->setCurrentRow(qMin(row, m_list->count() - 1));
    updateOrderButtons();
    updatePreview();
}

void CollageDialog::moveImage(int step)
{
    const int row = m_list->currentRow();
    const int target = row + step;
    if (row < 0 || target < 0 || target >= m_list->count())
        return;
    m_images.move(row, target);
    m_list->insertItem(target, m_list->takeItem(row));
    m_list->setCurrentRow(target);
    updateOrderButtons();
    updatePreview();
}

void CollageDialog::updateOrderButtons()
{
    const int row = m_list->currentRow();
    m_remove->setEnabled(row >= 0);
    m_moveUp->setEnabled(row > 0);
    m_moveDown->setEnabled(row >= 0 && row + 1 < m_list->count());
}

ImageCollage::Direction CollageDialog::direction() const
{
    return m_direction->currentIndex() == 0 ? ImageCollage::Direction::Horizontal
                                            : ImageCollage::Direction::Vertical;
}

void CollageDialog::updatePreview()
{
    if (m_images.isEmpty()) {
        m_preview->setImage({}, tr("拖入图片，或点击“添加图片”"));
        m_summary->clear();
        m_export->setEnabled(false);
        return;
    }

    QString error;
    const QSize size = ImageCollage::outputSize(m_images, direction(), m_spacing->value(), &error);
    if (size.isEmpty()) {
        m_preview->setImage({}, error);
        m_summary->clear();
        m_export->setEnabled(false);
        return;
    }

    QImage preview = ImageCollage::preview(m_images, direction(), m_spacing->value(), kPreviewBounds);
    m_preview->setImage(std::move(preview), tr("预览暂不可用。"));
    m_summary->setText(tr("%1 张图片 · %2 × %3 像素 · 透明背景")
                           .arg(m_images.size()).arg(size.width()).arg(size.height()));
    m_export->setEnabled(true);
}

void CollageDialog::exportPng()
{
    QFileDialog dialog(this, tr("导出拼图"), Config::effectiveSaveDir(), tr("PNG 图片 (*.png)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("png"));
    dialog.selectFile(tr("拼图.png"));
    QString path;
    while (dialog.exec() == QDialog::Accepted) {
        path = dialog.selectedFiles().value(0);
        if (QFileInfo(path).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0)
            break;
        QMessageBox::warning(this, tr("导出拼图"), tr("请使用 .png 文件名。"));
        path.clear();
    }
    if (path.isEmpty())
        return;

    QString error;
    const QImage result = ImageCollage::compose(m_images, direction(), m_spacing->value(), &error);
    if (result.isNull() || !ImageCollage::writePng(result, path, &error)) {
        QMessageBox::warning(this, tr("导出失败"), error);
        return;
    }
    Config::rememberSaveDir(QFileInfo(path).absolutePath());
    m_status->setText(tr("已导出 %1").arg(QFileInfo(path).fileName()));
    m_status->setToolTip(QDir::toNativeSeparators(path));
}
