#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

// Screenshot "beautify": the image on a background with margin, rounded corners and a soft
// shadow, ready for documents and posts.
namespace Beautify {

struct Options
{
    int background = 1; // 0 transparent, 1.. see backgroundNames()
    int padding = 48;   // logical px around the image
    int radius = 12;    // logical px corner radius
    bool shadow = true;
};

QStringList backgroundNames(); // index = Options::background

Options loadOptions();
void saveOptions(const Options &options);

// Renders at the image's device pixel ratio, so a HiDPI capture stays sharp.
QImage render(const QImage &source, const Options &options);

} // namespace Beautify
