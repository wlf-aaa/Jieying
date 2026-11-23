#pragma once

#include "config.h"

#include <QImage>
#include <QStringList>

namespace TableOcr {

struct Result
{
    QList<QStringList> cells;
    QString error;
    bool hasGrid = false;
};

// Blocking; call on a worker thread. Images without a complete rectangular grid use one column.
Result recognize(const QImage &image, Config::OcrEngine engine);

// Embedded tabs and line breaks are flattened to keep the clipboard grid rectangular.
QString toTsv(const QList<QStringList> &cells);

} // namespace TableOcr
