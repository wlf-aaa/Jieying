#include "tableocrdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

TableOcrDialog::TableOcrDialog(const QImage &image, Config::OcrEngine engine, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowStaysOnTopHint)
{
    setWindowTitle(tr("表格识别"));
    resize(700, 440);

    m_status = new QLabel(tr("正在识别表格…"), this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_table = new QTableWidget(this);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                             | QAbstractItemView::AnyKeyPressed);
    m_table->horizontalHeader()->setDefaultSectionSize(140);

    m_copy = new QPushButton(tr("复制到 Excel"), this);
    m_copy->setObjectName(QStringLiteral("copyTable"));
    m_copy->setEnabled(false);
    m_copy->setAutoDefault(false);
    m_copy->setToolTip(tr("单元格内的换行和制表符会在复制时转换为空格。"));
    auto *close = new QPushButton(tr("关闭"), this);
    close->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_copy, &QPushButton::clicked, this, &TableOcrDialog::copyTable);
    connect(m_table, &QTableWidget::itemChanged, this, &TableOcrDialog::updateCopyState);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_copy);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttons);

    connect(&m_watcher, &QFutureWatcher<TableOcr::Result>::finished, this, [this] { showResult(m_watcher.result()); });
    m_watcher.setFuture(QtConcurrent::run([image, engine] { return TableOcr::recognize(image, engine); }));
}

void TableOcrDialog::showResult(const TableOcr::Result &result)
{
    if (!result.error.isEmpty()) {
        m_status->setText(tr("表格识别失败。%1").arg(result.error));
        return;
    }
    if (result.cells.isEmpty()) {
        m_status->setText(tr("没有识别到表格或文字。"));
        return;
    }

    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(int(result.cells.size()));
    m_table->setColumnCount(int(result.cells.first().size()));
    m_table->horizontalHeader()->setStretchLastSection(true);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto &values = result.cells[row];
        for (int column = 0; column < values.size(); ++column) {
            if (!values[column].isEmpty())
                m_table->setItem(row, column, new QTableWidgetItem(values[column]));
        }
    }
    m_table->resizeRowsToContents();

    updateCopyState();
    if (!result.hasGrid) {
        m_status->setText(tr("未找到完整的表格边框，已将文字按行列在单列中。双击单元格可修改。"));
    } else if (m_copy->isEnabled()) {
        m_status->setText(tr("识别出 %1 行 × %2 列。双击单元格可修改，按 Enter 完成编辑。")
                              .arg(m_table->rowCount()).arg(m_table->columnCount()));
    } else {
        m_status->setText(tr("找到表格边框，单元格中没有识别到文字。双击单元格可填写，按 Enter 完成编辑。"));
    }
}

void TableOcrDialog::updateCopyState()
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        for (int column = 0; column < m_table->columnCount(); ++column) {
            const auto *item = m_table->item(row, column);
            if (item && !item->text().trimmed().isEmpty()) {
                m_copy->setEnabled(true);
                return;
            }
        }
    }
    m_copy->setEnabled(false);
}

void TableOcrDialog::copyTable()
{
    m_copy->setFocus();
    QList<QStringList> cells;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QStringList values;
        for (int column = 0; column < m_table->columnCount(); ++column) {
            const auto *item = m_table->item(row, column);
            values.append(item ? item->text() : QString());
        }
        cells.append(values);
    }
    QApplication::clipboard()->setText(TableOcr::toTsv(cells));
    m_status->setText(tr("已复制。请在 Excel 中选中起始单元格后粘贴。"));
    emit copied();
}
