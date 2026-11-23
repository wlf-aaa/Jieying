#include "historydialog.h"

#include "capturehistory.h"
#include "config.h"
#include "icons.h"
#include "imageio.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

constexpr QSize kThumbnailSize(96, 64);

QString entryLabel(const QFileInfo &entry)
{
    const QString baseName = entry.completeBaseName();
    const QDateTime timestamp = QDateTime::fromString(baseName.left(19), QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    return timestamp.isValid() ? timestamp.toString(QStringLiteral("MM-dd HH:mm:ss")) : baseName;
}

class HistoryItem final : public QListWidgetItem
{
public:
    HistoryItem(CaptureHistory *history, const QFileInfo &entry)
        : QListWidgetItem(entryLabel(entry))
        , m_history(history)
        , m_entry(entry)
    {
        setData(Qt::UserRole, entry.absoluteFilePath());
        setToolTip(entry.fileName());
        setSizeHint(QSize(0, kThumbnailSize.height() + 8));
    }

    QVariant data(int role) const override
    {
        // A history can contain hundreds of large PNGs. Decode thumbnails as rows are shown.
        if (role == Qt::DecorationRole && m_history)
            return m_history->thumbnail(m_entry);
        return QListWidgetItem::data(role);
    }

private:
    QPointer<CaptureHistory> m_history;
    QFileInfo m_entry;
};

} // namespace

class HistoryPreview final : public QWidget
{
public:
    using QWidget::QWidget;

    void display(const QPixmap &image, const QString &message)
    {
        m_image = image;
        m_message = message;
        update();
    }

    const QPixmap &image() const { return m_image; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::AlternateBase));
        const QRect area = rect().marginsRemoved(QMargins(12, 12, 12, 12));
        if (m_image.isNull()) {
            painter.setPen(palette().color(QPalette::Text));
            painter.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, m_message);
            return;
        }

        const QSize size = m_image.size().scaled(area.size(), Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        QRect target(QPoint(), size);
        target.moveCenter(area.center());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(target, m_image);
    }

private:
    QPixmap m_image;
    QString m_message;
};

HistoryDialog::HistoryDialog(CaptureHistory *history, QWidget *parent)
    : QDialog(parent)
    , m_history(history)
    , m_list(new QListWidget(this))
    , m_preview(new HistoryPreview(this))
    , m_dimensions(new QLabel(this))
    , m_copy(new QPushButton(toolIcon("copy"), tr("复制"), this))
    , m_saveAs(new QPushButton(toolIcon("save"), tr("另存为…"), this))
    , m_pin(new QPushButton(toolIcon("pin"), tr("贴到桌面"), this))
    , m_collage(new QPushButton(tr("拼排所选…"), this))
{
    setWindowTitle(tr("截图历史"));
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    resize(900, 600);

    m_list->setObjectName(QStringLiteral("historyList"));
    m_list->setAccessibleName(tr("截图记录"));
    m_list->setIconSize(kThumbnailSize);
    m_list->setUniformItemSizes(true);
    m_list->setMinimumWidth(220);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_preview->setObjectName(QStringLiteral("historyPreview"));
    m_preview->setAccessibleName(tr("截图预览"));
    m_preview->setMinimumSize(300, 220);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_list);
    splitter->addWidget(m_preview);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({240, 660});

    m_copy->setObjectName(QStringLiteral("historyCopy"));
    m_saveAs->setObjectName(QStringLiteral("historySaveAs"));
    m_pin->setObjectName(QStringLiteral("historyPin"));
    m_collage->setObjectName(QStringLiteral("historyCollage"));
    m_copy->setToolTip(tr("复制到剪贴板（Ctrl+C）"));
    m_saveAs->setToolTip(tr("另存为（Ctrl+S）"));
    m_collage->setToolTip(tr("按住 Ctrl 或 Shift 在列表中选中至少两张截图"));
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_dimensions);
    actions->addStretch();
    for (auto *button : {m_copy, m_saveAs, m_pin, m_collage}) {
        button->setAutoDefault(false);
        actions->addWidget(button);
    }

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addLayout(actions);

    connect(m_list, &QListWidget::currentItemChanged, this, &HistoryDialog::updateSelection);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &HistoryDialog::updateCollageAction);
    connect(m_copy, &QPushButton::clicked, this, &HistoryDialog::copySelection);
    connect(m_saveAs, &QPushButton::clicked, this, &HistoryDialog::saveSelection);
    connect(m_pin, &QPushButton::clicked, this, &HistoryDialog::pinSelection);
    connect(m_collage, &QPushButton::clicked, this, [this] {
        const QStringList paths = selectedPaths();
        if (paths.size() >= 2)
            emit collageRequested(paths);
    });
    new QShortcut(QKeySequence::Copy, this, this, &HistoryDialog::copySelection);
    new QShortcut(QKeySequence::Save, this, this, &HistoryDialog::saveSelection);
    if (m_history) {
        connect(m_history, &CaptureHistory::changed, this, &HistoryDialog::refreshEntries);
        connect(m_history, &QObject::destroyed, this, &HistoryDialog::refreshEntries);
    }

    refreshEntries();
    m_list->setFocus();
}

QString HistoryDialog::currentPath() const
{
    const auto *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QStringList HistoryDialog::selectedPaths() const
{
    QStringList paths;
    for (int row = 0; row < m_list->count(); ++row) {
        const auto *item = m_list->item(row);
        if (item->isSelected())
            paths.append(item->data(Qt::UserRole).toString());
    }
    return paths;
}

void HistoryDialog::refreshEntries()
{
    const QString previewPath = currentPath();
    const QStringList paths = selectedPaths();
    const QSet<QString> selected(paths.cbegin(), paths.cend());
    const QSignalBlocker block(m_list);
    m_list->clear();
    QListWidgetItem *current = nullptr;
    QListWidgetItem *firstSelected = nullptr;
    if (m_history) {
        for (const QFileInfo &entry : m_history->entries()) {
            auto *item = new HistoryItem(m_history, entry);
            m_list->addItem(item);
            const QString path = entry.absoluteFilePath();
            if (path == previewPath)
                current = item;
            if (selected.contains(path)) {
                item->setSelected(true);
                if (!firstSelected)
                    firstSelected = item;
            }
        }
    }
    if (firstSelected)
        m_list->setCurrentItem(current ? current : firstSelected, QItemSelectionModel::NoUpdate);
    else
        m_list->setCurrentItem(current ? current : m_list->item(0), QItemSelectionModel::ClearAndSelect);
    updateSelection();
    updateCollageAction();
}

void HistoryDialog::updateSelection()
{
    const QString path = currentPath();
    QPixmap image;
    QString message;
    if (path.isEmpty()) {
        message = Config::historySize() > 0 ? tr("还没有截图记录") : tr("截图历史已在设置中关闭");
    } else {
        image = QPixmap::fromImage(QImageReader(path).read());
        if (image.isNull())
            message = tr("无法加载此截图，文件可能已删除或损坏。");
    }
    m_preview->display(image, message);
    m_dimensions->setText(image.isNull() ? QString() : tr("%1 × %2 像素").arg(image.width()).arg(image.height()));
    for (auto *button : {m_copy, m_saveAs, m_pin})
        button->setEnabled(!image.isNull());
}

void HistoryDialog::updateCollageAction()
{
    m_collage->setEnabled(m_list->selectedItems().size() >= 2);
}

QPixmap HistoryDialog::selectedImage()
{
    if (m_preview->image().isNull())
        return {};
    if (!QFileInfo::exists(currentPath())) {
        refreshEntries();
        return {};
    }
    return m_preview->image();
}

void HistoryDialog::copySelection()
{
    const QPixmap image = selectedImage();
    if (!image.isNull())
        ImageIO::copyToClipboard(image);
}

void HistoryDialog::saveSelection()
{
    const QPixmap image = selectedImage();
    if (!image.isNull())
        ImageIO::saveImageAs(image, this);
}

void HistoryDialog::pinSelection()
{
    const QPixmap image = selectedImage();
    if (!image.isNull())
        emit pinRequested(image);
}
