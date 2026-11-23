#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <windows.h>

#include <dpapi.h>

namespace Config {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Config", text);
}

const char *hotkeyKey(Hotkey hotkey)
{
    switch (hotkey) {
    case Hotkey::Capture:
        return "hotkeys/capture";
    case Hotkey::PinClipboard:
        return "hotkeys/pinClipboard";
    case Hotkey::LongShot:
        return "hotkeys/longShot";
    case Hotkey::RecordGif:
        return "hotkeys/recordGif";
    case Hotkey::RecordMp4:
        return "hotkeys/recordMp4";
    case Hotkey::TogglePins:
        return "hotkeys/togglePins";
    case Hotkey::CaptureScreen:
        return "hotkeys/captureScreen";
    case Hotkey::CaptureWindow:
        return "hotkeys/captureWindow";
    case Hotkey::RepeatCapture:
        return "hotkeys/repeatCapture";
    case Hotkey::NextPinGroup:
        return "hotkeys/nextPinGroup";
    }
    return "";
}

QVariant read(const char *key, const QVariant &fallback)
{
    return QSettings().value(QLatin1String(key), fallback);
}

void write(const char *key, const QVariant &value)
{
    QSettings().setValue(QLatin1String(key), value);
}

} // namespace

QString hotkeyName(Hotkey hotkey)
{
    switch (hotkey) {
    case Hotkey::Capture:
        return tr("截图");
    case Hotkey::PinClipboard:
        return tr("贴剪贴板图片");
    case Hotkey::LongShot:
        return tr("长截图");
    case Hotkey::RecordGif:
        return tr("录制 GIF");
    case Hotkey::RecordMp4:
        return tr("录制 MP4");
    case Hotkey::TogglePins:
        return tr("隐藏/显示当前贴图");
    case Hotkey::NextPinGroup:
        return tr("切换贴图分组");
    case Hotkey::CaptureScreen:
        return tr("截取全屏");
    case Hotkey::CaptureWindow:
        return tr("截取当前窗口");
    case Hotkey::RepeatCapture:
        return tr("重截上次选区");
    }
    return {};
}

QKeySequence hotkey(Hotkey hotkey)
{
    return QKeySequence(read(hotkeyKey(hotkey), schemeHotkey(HotkeyScheme::Default, hotkey).toString()).toString());
}

QString hotkeySchemeName(HotkeyScheme scheme)
{
    switch (scheme) {
    case HotkeyScheme::Default:
        return tr("方案一（F1 / F3）");
    case HotkeyScheme::Alternate:
        return tr("方案二（Ctrl+1 / Ctrl+2）");
    case HotkeyScheme::Custom:
        return tr("自定义");
    }
    return {};
}

QKeySequence schemeHotkey(HotkeyScheme scheme, Hotkey hotkey)
{
    if (hotkey == Hotkey::NextPinGroup)
        return scheme == HotkeyScheme::Custom ? QKeySequence() : QKeySequence(QStringLiteral("Ctrl+F3"));
    switch (scheme) {
    case HotkeyScheme::Default:
        if (hotkey == Hotkey::Capture)
            return QKeySequence(QStringLiteral("F1"));
        if (hotkey == Hotkey::PinClipboard)
            return QKeySequence(QStringLiteral("F3"));
        if (hotkey == Hotkey::TogglePins)
            return QKeySequence(QStringLiteral("Shift+F3"));
        return {};
    case HotkeyScheme::Alternate:
        if (hotkey == Hotkey::Capture)
            return QKeySequence(QStringLiteral("Ctrl+1"));
        if (hotkey == Hotkey::PinClipboard)
            return QKeySequence(QStringLiteral("Ctrl+2"));
        return {};
    case HotkeyScheme::Custom:
        return {};
    }
    return {};
}

HotkeyScheme currentHotkeyScheme()
{
    for (HotkeyScheme scheme : kHotkeySchemes) {
        bool same = true;
        for (Hotkey key : kHotkeys)
            same = same && hotkey(key) == schemeHotkey(scheme, key);
        if (same)
            return scheme;
    }
    return HotkeyScheme::Custom;
}

void setHotkey(Hotkey hotkey, const QKeySequence &sequence)
{
    write(hotkeyKey(hotkey), sequence.toString());
}

bool pauseHotkeysInFullscreen()
{
    return read("hotkeys/pauseInFullscreen", false).toBool();
}

void setPauseHotkeysInFullscreen(bool pause)
{
    write("hotkeys/pauseInFullscreen", pause);
}

ConfirmAction confirmAction()
{
    return ConfirmAction(qBound(0, read("capture/confirmAction", 0).toInt(), 2));
}

void setConfirmAction(ConfirmAction action)
{
    write("capture/confirmAction", int(action));
}

bool showMagnifier()
{
    return read("capture/magnifier", true).toBool();
}

void setShowMagnifier(bool show)
{
    write("capture/magnifier", show);
}

int historySize()
{
    return qBound(0, read("capture/historySize", 20).toInt(), 200);
}

void setHistorySize(int count)
{
    write("capture/historySize", count);
}

PinWheelAction pinWheelAction(bool control)
{
    const auto fallback = control ? PinWheelAction::Opacity : PinWheelAction::Zoom;
    bool ok = false;
    const int action = read(control ? "pin/ctrlWheel" : "pin/wheel", int(fallback)).toInt(&ok);
    return ok && action >= int(PinWheelAction::Zoom) && action <= int(PinWheelAction::Disabled)
               ? PinWheelAction(action) : fallback;
}

void setPinWheelAction(bool control, PinWheelAction action)
{
    write(control ? "pin/ctrlWheel" : "pin/wheel", int(action));
}

QString saveDir()
{
    return read("save/dir", QString()).toString();
}

void setSaveDir(const QString &dir)
{
    write("save/dir", dir);
}

QString effectiveSaveDir()
{
    const QString fixed = saveDir();
    if (!fixed.isEmpty())
        return fixed;
    return read("save/lastDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
}

void rememberSaveDir(const QString &dir)
{
    write("save/lastDir", dir);
}

bool askWhereToSave()
{
    return read("save/ask", true).toBool();
}

void setAskWhereToSave(bool ask)
{
    write("save/ask", ask);
}

bool autoSaveCaptures()
{
    return read("save/auto", false).toBool();
}

void setAutoSaveCaptures(bool on)
{
    write("save/auto", on);
}

QString imageFormat()
{
    const QString format = read("save/format", QStringLiteral("png")).toString();
    return format == QLatin1String("jpg") || format == QLatin1String("webp") ? format : QStringLiteral("png");
}

void setImageFormat(const QString &format)
{
    write("save/format", format);
}

int jpgQuality()
{
    return qBound(1, read("save/jpgQuality", 90).toInt(), 100);
}

void setJpgQuality(int quality)
{
    write("save/jpgQuality", quality);
}

QString defaultFileNameTemplate()
{
    return QStringLiteral("Jieying_{yyyyMMdd_HHmmss}");
}

QString fileNameTemplate()
{
    const QString pattern = read("save/fileName", defaultFileNameTemplate()).toString().trimmed();
    return pattern.isEmpty() ? defaultFileNameTemplate() : pattern;
}

void setFileNameTemplate(const QString &pattern)
{
    write("save/fileName", pattern);
}

QString expandFileName(const QString &pattern, const QString &suffix, const QDateTime &when)
{
    static const QRegularExpression field(QStringLiteral("\\{([^{}]*)\\}"));
    static const QRegularExpression forbidden(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1F]"));
    QString name;
    qsizetype last = 0;
    for (auto it = field.globalMatch(pattern); it.hasNext();) {
        const auto match = it.next();
        name += pattern.mid(last, match.capturedStart() - last);
        name += when.toString(match.captured(1));
        last = match.capturedEnd();
    }
    name += pattern.mid(last);
    name.replace(forbidden, QStringLiteral("_"));
    name = name.trimmed();
    if (name.isEmpty())
        name = when.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    return name + QLatin1Char('.') + suffix;
}

QList<QColor> annotationPalette()
{
    return {QColor("#FF3B30"), QColor("#FF9500"), QColor("#FFCC00"), QColor("#34C759"),
            QColor("#007AFF"), QColor("#AF52DE"), QColor("#000000"), QColor("#FFFFFF")};
}

QColor annotationColor()
{
    const QColor color(read("annotate/color", QStringLiteral("#FF3B30")).toString());
    return color.isValid() ? color : annotationPalette().first();
}

void setAnnotationColor(const QColor &color)
{
    write("annotate/color", color.name());
}

int annotationSize()
{
    return qBound(0, read("annotate/size", 1).toInt(), 2);
}

void setAnnotationSize(int level)
{
    write("annotate/size", level);
}

bool annotationFill()
{
    return read("annotate/fill", false).toBool();
}

void setAnnotationFill(bool fill)
{
    write("annotate/fill", fill);
}

TranslateProvider translateProvider()
{
    return TranslateProvider(qBound(0, read("translate/provider", 0).toInt(), 2));
}

void setTranslateProvider(TranslateProvider provider)
{
    write("translate/provider", int(provider));
}

QString baiduAppId()
{
    return read("translate/baiduAppId", QString()).toString().trimmed();
}

void setBaiduAppId(const QString &id)
{
    write("translate/baiduAppId", id.trimmed());
}

QString tencentSecretId()
{
    return read("translate/tencentSecretId", QString()).toString().trimmed();
}

void setTencentSecretId(const QString &id)
{
    write("translate/tencentSecretId", id.trimmed());
}

QString tencentRegion()
{
    const QString region = read("translate/tencentRegion", QString()).toString().trimmed();
    return region.isEmpty() ? QStringLiteral("ap-guangzhou") : region;
}

void setTencentRegion(const QString &region)
{
    write("translate/tencentRegion", region.trimmed());
}

QString translateSecret(const char *name)
{
    const QByteArray key = QByteArray("translate/") + name;
    const QByteArray sealed = QByteArray::fromBase64(read(key.constData(), QString()).toString().toLatin1());
    if (sealed.isEmpty())
        return {};
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE *>(const_cast<char *>(sealed.constData()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    const QString value = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return value;
}

void setTranslateSecret(const char *name, const QString &value)
{
    const QByteArray key = QByteArray("translate/") + name;
    const QByteArray plain = value.trimmed().toUtf8();
    if (plain.isEmpty()) {
        write(key.constData(), QString());
        return;
    }
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Jieying translation key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return;
    write(key.constData(), QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(out.pbData), int(out.cbData)).toBase64()));
    LocalFree(out.pbData);
}

bool translateConsented()
{
    return read("translate/consented", false).toBool();
}

void setTranslateConsented(bool consented)
{
    write("translate/consented", consented);
}

OcrEngine ocrEngine()
{
    return OcrEngine(qBound(0, read("ocr/engine", 0).toInt(), 2));
}

void setOcrEngine(OcrEngine engine)
{
    write("ocr/engine", int(engine));
}

bool ocrAutoCopy()
{
    return read("ocr/autoCopy", false).toBool();
}

void setOcrAutoCopy(bool autoCopy)
{
    write("ocr/autoCopy", autoCopy);
}

int gifFps()
{
    return qBound(5, read("record/gifFps", 15).toInt(), 30);
}

void setGifFps(int fps)
{
    write("record/gifFps", fps);
}

int mp4Fps()
{
    return qBound(10, read("record/mp4Fps", 30).toInt(), 60);
}

void setMp4Fps(int fps)
{
    write("record/mp4Fps", fps);
}

VideoQuality mp4Quality()
{
    return VideoQuality(qBound(0, read("record/mp4Quality", 1).toInt(), 2));
}

void setMp4Quality(VideoQuality quality)
{
    write("record/mp4Quality", int(quality));
}

double bitsPerPixel(VideoQuality quality)
{
    switch (quality) {
    case VideoQuality::Low:
        return 0.08;
    case VideoQuality::Medium:
        return 0.15; // keeps screen text crisp
    case VideoQuality::High:
        return 0.3;
    }
    return 0.15;
}

bool recordSystemAudio()
{
    return read("record/systemAudio", true).toBool();
}

void setRecordSystemAudio(bool on)
{
    write("record/systemAudio", on);
}

bool recordMicrophone()
{
    return read("record/microphone", false).toBool();
}

void setRecordMicrophone(bool on)
{
    write("record/microphone", on);
}

bool recordPanelCollapsed()
{
    return read("record/panelCollapsed", false).toBool();
}

void setRecordPanelCollapsed(bool on)
{
    write("record/panelCollapsed", on);
}

bool recordClicks()
{
    return read("record/clicks", true).toBool();
}

void setRecordClicks(bool on)
{
    write("record/clicks", on);
}

bool recordKeys()
{
    return read("record/keys", false).toBool();
}

void setRecordKeys(bool on)
{
    write("record/keys", on);
}

bool recordCursor()
{
    return read("record/cursor", true).toBool();
}

void setRecordCursor(bool record)
{
    write("record/cursor", record);
}

CaptureBackend captureBackend()
{
    return CaptureBackend(qBound(0, read("record/backend", 0).toInt(), 2));
}

void setCaptureBackend(CaptureBackend backend)
{
    write("record/backend", int(backend));
}

} // namespace Config
