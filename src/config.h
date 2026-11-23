#pragma once

#include <QColor>
#include <QDateTime>
#include <QKeySequence>
#include <QList>
#include <QString>

// Typed access to the persisted settings (QSettings, i.e. HKCU\Software\Jieying\Jieying),
// with every default defined once, here. Safe to read from any thread.
namespace Config {

// ---- hotkeys --------------------------------------------------------------------------------

enum class Hotkey { Capture, PinClipboard, LongShot, RecordGif, RecordMp4, TogglePins, CaptureScreen, CaptureWindow,
                    NextPinGroup, RepeatCapture };
inline constexpr Hotkey kHotkeys[] = {Hotkey::Capture,       Hotkey::CaptureScreen, Hotkey::CaptureWindow,
                                      Hotkey::RepeatCapture,
                                      Hotkey::PinClipboard,  Hotkey::TogglePins,    Hotkey::NextPinGroup,
                                      Hotkey::LongShot,
                                      Hotkey::RecordGif,     Hotkey::RecordMp4};

QString hotkeyName(Hotkey hotkey); // for display, e.g. "截图"
QKeySequence hotkey(Hotkey hotkey);  // empty when unassigned
void setHotkey(Hotkey hotkey, const QKeySequence &sequence);

enum class HotkeyScheme { Default, Alternate, Custom };
inline constexpr HotkeyScheme kHotkeySchemes[] = {HotkeyScheme::Default, HotkeyScheme::Alternate};
QString hotkeySchemeName(HotkeyScheme scheme);
QKeySequence schemeHotkey(HotkeyScheme scheme, Hotkey hotkey); // empty: unassigned
// The scheme the current keys match exactly, else Custom.
HotkeyScheme currentHotkeyScheme();

bool pauseHotkeysInFullscreen();
void setPauseHotkeysInFullscreen(bool pause);

// ---- capture --------------------------------------------------------------------------------

// What Enter / double-click inside the selection does.
enum class ConfirmAction { Copy, Save, Pin };
ConfirmAction confirmAction();
void setConfirmAction(ConfirmAction action);

bool showMagnifier();
void setShowMagnifier(bool show);

// How many recent captures to keep for re-pinning (0 = keep none).
int historySize();
void setHistorySize(int count);

enum class PinWheelAction { Zoom, Opacity, Disabled };
PinWheelAction pinWheelAction(bool control);
void setPinWheelAction(bool control, PinWheelAction action);

// ---- saving ---------------------------------------------------------------------------------

// Folder for saved images and recordings; empty means "the folder used last time".
QString saveDir();
void setSaveDir(const QString &dir);
// Folder a save starts in: saveDir(), else the last used one, else Pictures.
QString effectiveSaveDir();
void rememberSaveDir(const QString &dir);

// false: save straight into effectiveSaveDir() under the template name, without a dialog.
bool askWhereToSave();
void setAskWhereToSave(bool ask);
// Every capture (copied, pinned, recognized...) also lands in the save folder.
bool autoSaveCaptures();
void setAutoSaveCaptures(bool on);

QString imageFormat(); // "png", "jpg" or "webp"
void setImageFormat(const QString &format);
int jpgQuality(); // 1..100
void setJpgQuality(int quality);

// File name without suffix; text in {braces} is a QDateTime format, e.g. "Jieying_{yyyyMMdd_HHmmss}".
QString fileNameTemplate();
void setFileNameTemplate(const QString &pattern);
QString defaultFileNameTemplate();
// Expands the template and appends ".suffix"; characters Windows forbids become '_'.
QString expandFileName(const QString &pattern, const QString &suffix, const QDateTime &when);

// ---- annotation -----------------------------------------------------------------------------

QList<QColor> annotationPalette();
QColor annotationColor();
void setAnnotationColor(const QColor &color);
int annotationSize(); // 0 small, 1 medium, 2 large
void setAnnotationSize(int level);
bool annotationFill(); // solid shapes, text on a colored box
void setAnnotationFill(bool fill);

// ---- translation ----------------------------------------------------------------------------

enum class TranslateProvider { None, Baidu, Tencent };
TranslateProvider translateProvider();
void setTranslateProvider(TranslateProvider provider);
QString baiduAppId();
void setBaiduAppId(const QString &id);
QString tencentSecretId();
void setTencentSecretId(const QString &id);
QString tencentRegion(); // default ap-guangzhou
void setTencentRegion(const QString &region);
// Secret keys ("baiduKey", "tencentKey"), stored encrypted for this Windows user (DPAPI).
QString translateSecret(const char *name);
void setTranslateSecret(const char *name, const QString &value);
// The user has agreed that recognized text is sent to the provider.
bool translateConsented();
void setTranslateConsented(bool consented);

// ---- OCR ------------------------------------------------------------------------------------

enum class OcrEngine { Auto, Paddle, Windows };
OcrEngine ocrEngine();
void setOcrEngine(OcrEngine engine);
bool ocrAutoCopy(); // copy the text as soon as recognition finishes
void setOcrAutoCopy(bool autoCopy);

// ---- recording ------------------------------------------------------------------------------

int gifFps(); // 5..30
void setGifFps(int fps);
int mp4Fps(); // 10..60
void setMp4Fps(int fps);
enum class VideoQuality { Low, Medium, High };
VideoQuality mp4Quality();
void setMp4Quality(VideoQuality quality);
double bitsPerPixel(VideoQuality quality); // H.264 bitrate per pixel per frame
bool recordSystemAudio(); // MP4: what the speakers play (default on)
void setRecordSystemAudio(bool on);
bool recordMicrophone(); // MP4: the default microphone (default off)
void setRecordMicrophone(bool on);
bool recordPanelCollapsed(); // the recording panel shows only the time (default off)
void setRecordPanelCollapsed(bool on);
bool recordClicks(); // rings where the mouse is pressed (default on)
void setRecordClicks(bool on);
bool recordKeys(); // shortcut captions (default off)
void setRecordKeys(bool on);
bool recordCursor();
void setRecordCursor(bool record);

// How recording grabs frames. Auto: Windows.Graphics.Capture where its border can be hidden
// (Windows 11), else GDI unless GDI is too slow for the region and frame rate.
enum class CaptureBackend { Auto, Wgc, Gdi };
CaptureBackend captureBackend();
void setCaptureBackend(CaptureBackend backend);

} // namespace Config
