#pragma once

#include "tableocr.h"

#include <QDialog>
#include <QFutureWatcher>

class QLabel;
class QPushButton;
class QTableWidget;

class TableOcrDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TableOcrDialog(const QImage &image, Config::OcrEngine engine, QWidget *parent = nullptr);

signals:
    void copied();

private:
    void showResult(const TableOcr::Result &result);
    void updateCopyState();
    void copyTable();

    QLabel *m_status;
    QTableWidget *m_table;
    QPushButton *m_copy;
    QFutureWatcher<TableOcr::Result> m_watcher;
};
