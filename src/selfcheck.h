#pragma once

#include <QString>

// `jieying --self-check <report>`: checks an installation without showing any window or
// registering hotkeys — Qt plugins and translation, both OCR engines, GIF/MP4 encoding,
// where settings are stored — and writes a UTF-8 report. Returns 0 when everything needed works.
int runSelfCheck(const QString &reportPath);
