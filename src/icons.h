#pragma once

#include <QIcon>

// Toolbar and menu icons: assets/icons/<name>.svg, embedded as Qt resources (24×24 line icons,
// rendered sharp at any scale factor).
inline QIcon toolIcon(const char *name)
{
    return QIcon(QStringLiteral(":/icons/%1.svg").arg(QLatin1String(name)));
}
