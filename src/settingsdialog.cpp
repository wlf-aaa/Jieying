#include "settingsdialog.h"

#include "translator.h"

#include "autostart.h"
#include "hotkeymanager.h"
#include "wgcgrabber.h"
#ifdef JIEYING_HAVE_PADDLEOCR
#include "paddleocr.h"
#endif

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet("color: #777;");
    return label;
}

QWidget *page(QWidget *parent, QFormLayout *&form)
{
    auto *widget = new QWidget(parent);
    form = new QFormLayout(widget);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setVerticalSpacing(12);
    form->setContentsMargins(20, 16, 20, 16);
    return widget;
}

template <typename Enum>
QComboBox *enumCombo(const QStringList &items, Enum current, QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->addItems(items);
    combo->setCurrentIndex(int(current));
    return combo;
}

} // namespace

SettingsDialog::SettingsDialog(HotkeyManager *hotkeys, QWidget *parent)
    : QDialog(parent, Qt::Window | Qt::WindowCloseButtonHint)
    , m_hotkeys(hotkeys)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("设置")); // Qt appends " - 截影"
    resize(640, 440);

    m_sidebar = new QListWidget(this);
    m_sidebar->setFixedWidth(130);
    m_sidebar->setStyleSheet("QListWidget { border: none; background: #f3f4f6; padding-top: 8px; }"
                             "QListWidget::item { padding: 8px 14px; }"
                             "QListWidget::item:selected { background: #dbe9ff; color: #0b57d0; }");
    m_pages = new QStackedWidget(this);
    connect(m_sidebar, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    addPage(tr("常规"), buildGeneralPage());
    addPage(tr("快捷键"), buildHotkeyPage());
    addPage(tr("保存"), buildSavePage());
    addPage(tr("标注"), buildAnnotationPage());
    addPage(tr("文字识别"), buildOcrPage());
    addPage(tr("翻译"), buildTranslatePage());
    addPage(tr("录屏"), buildRecordingPage());
    m_sidebar->setCurrentRow(0);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_sidebar);
    layout->addWidget(m_pages, 1);
}

SettingsDialog::~SettingsDialog()
{
    m_hotkeys->setSuspended(false); // in case the window closed while a key field had focus
}

QStringList SettingsDialog::pageTitles() const
{
    QStringList titles;
    for (int i = 0; i < m_sidebar->count(); ++i)
        titles << m_sidebar->item(i)->text();
    return titles;
}

void SettingsDialog::showPage(int index)
{
    m_sidebar->setCurrentRow(index);
}

void SettingsDialog::addPage(const QString &title, QWidget *page)
{
    m_sidebar->addItem(title);
    m_pages->addWidget(page);
}

QWidget *SettingsDialog::buildGeneralPage()
{
    QFormLayout *form = nullptr;
    QWidget *widget = page(this, form);

    auto *autostart = new QCheckBox(tr("开机时自动启动截影"), widget);
    autostart->setChecked(Autostart::isEnabled());
    connect(autostart, &QCheckBox::toggled, this, [this, autostart](bool on) {
        if (Autostart::setEnabled(on))
            return;
        const QSignalBlocker block(autostart);
        autostart->setChecked(Autostart::isEnabled());
        QMessageBox::warning(this, tr("开机自启"), tr("无法修改开机启动设置（注册表写入被拒绝）。"));
    });
    form->addRow(QString(), autostart);

    auto *confirm = enumCombo({tr("复制到剪贴板"), tr("保存"), tr("贴到桌面")}, Config::confirmAction(), widget);
    connect(confirm, &QComboBox::currentIndexChanged, this,
            [](int index) { Config::setConfirmAction(Config::ConfirmAction(index)); });
    form->addRow(tr("回车 / 双击选区："), confirm);

    auto *history = new QSpinBox(widget);
    history->setRange(0, 200);
    history->setSuffix(tr(" 张"));
    history->setSpecialValueText(tr("不保留"));
    history->setValue(Config::historySize());
    connect(history, &QSpinBox::valueChanged, this, &Config::setHistorySize);
    form->addRow(tr("保留最近截图："), history);
    form->addRow(QString(), hint(tr("从托盘菜单「最近截图」可打开历史面板。图片只保存在本机，卸载时删除。"), widget));

    auto *magnifier = new QCheckBox(tr("截图时显示放大镜和颜色值"), widget);
    magnifier->setChecked(Config::showMagnifier());
    connect(magnifier, &QCheckBox::toggled, this, &Config::setShowMagnifier);
    form->addRow(QString(), magnifier);

    const QStringList wheelActions{tr("缩放"), tr("调整不透明度"), tr("禁用")};
    for (bool control : {false, true}) {
        auto *action = enumCombo(wheelActions, Config::pinWheelAction(control), widget);
        action->setObjectName(control ? QStringLiteral("pinCtrlWheelAction") : QStringLiteral("pinWheelAction"));
        connect(action, &QComboBox::currentIndexChanged, this,
                [control](int index) { Config::setPinWheelAction(control, Config::PinWheelAction(index)); });
        form->addRow(control ? tr("贴图 Ctrl+滚轮：") : tr("贴图滚轮："), action);
    }
    return widget;
}

QWidget *SettingsDialog::buildHotkeyPage()
{
    QFormLayout *form = nullptr;
    QWidget *widget = page(this, form);

    // One-click key sets; editing any single key below turns the choice into "custom".
    m_scheme = new QComboBox(widget);
    for (Config::HotkeyScheme scheme : Config::kHotkeySchemes)
        m_scheme->addItem(Config::hotkeySchemeName(scheme), int(scheme));
    m_scheme->addItem(Config::hotkeySchemeName(Config::HotkeyScheme::Custom), int(Config::HotkeyScheme::Custom));
    connect(m_scheme, &QComboBox::activated, this, [this](int index) {
        const auto scheme = Config::HotkeyScheme(m_scheme->itemData(index).toInt());
        if (scheme == Config::HotkeyScheme::Custom)
            return; // nothing to switch to; it only describes edited keys
        m_hotkeys->applyScheme(scheme);
        refreshHotkeys();
    });
    form->addRow(tr("快捷键方案："), m_scheme);
    form->addRow(QString(), hint(tr("方案一可用 Shift+F3 隐藏或显示当前组贴图。两种方案都可按 Ctrl+F3 切换贴图分组。"
                                    "若按键被占用，可更换按键或关闭占用它的程序。"),
                                 widget));
    form->addRow(hint(tr("点击输入框后按下新的组合键；用 × 清除。全局生效，在任何程序里都能用。"), widget));

    auto *pauseFullscreen = new QCheckBox(tr("前台应用全屏时暂停全局快捷键"), widget);
    pauseFullscreen->setObjectName(QStringLiteral("pauseHotkeysInFullscreen"));
    pauseFullscreen->setChecked(Config::pauseHotkeysInFullscreen());
    connect(pauseFullscreen, &QCheckBox::toggled, this, [this](bool enabled) {
        Config::setPauseHotkeysInFullscreen(enabled);
        emit fullscreenHotkeyPauseEnabled(enabled);
    });
    form->addRow(pauseFullscreen);
    connect(m_hotkeys, &HotkeyManager::fullscreenPauseChanged, pauseFullscreen, [this] {
        for (Config::Hotkey hotkey : Config::kHotkeys)
            showHotkeyStatus(hotkey, {}, false);
    });

    for (Config::Hotkey hotkey : Config::kHotkeys) {
        auto *edit = new QKeySequenceEdit(Config::hotkey(hotkey), widget);
        m_hotkeyEdits.insert(hotkey, edit);
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        if (auto *field = edit->findChild<QLineEdit *>())
            field->setPlaceholderText(tr("点击后按下快捷键"));
        edit->installEventFilter(this); // suspend global hotkeys while capturing a key
        auto *status = new QLabel(widget);
        status->setMinimumWidth(170);
        status->setWordWrap(true);
        m_hotkeyStatus.insert(hotkey, status);

        auto apply = [this, hotkey, edit] {
            const QString error = m_hotkeys->change(hotkey, edit->keySequence());
            if (!error.isEmpty()) {
                const QSignalBlocker block(edit);
                edit->setKeySequence(Config::hotkey(hotkey)); // show the key that is still in effect
            }
            showHotkeyStatus(hotkey, error, true);
            showCurrentScheme();
        };
        connect(edit, &QKeySequenceEdit::editingFinished, this, apply);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [edit, apply](const QKeySequence &sequence) {
            if (sequence.isEmpty() && !edit->hasFocus()) // cleared with the × button
                apply();
        });

        auto *row = new QHBoxLayout;
        row->addWidget(edit, 1);
        row->addWidget(status);
        form->addRow(Config::hotkeyName(hotkey) + tr("："), row);
        showHotkeyStatus(hotkey, QString(), false);
    }
    form->addRow(hint(tr("录屏过程中再按一次截图或录屏快捷键会停止录制。"), widget));
    showCurrentScheme();
    return widget;
}

void SettingsDialog::showCurrentScheme()
{
    m_scheme->setCurrentIndex(m_scheme->findData(int(Config::currentHotkeyScheme())));
}

void SettingsDialog::refreshHotkeys()
{
    for (auto it = m_hotkeyEdits.cbegin(); it != m_hotkeyEdits.cend(); ++it) {
        const QSignalBlocker block(it.value());
        it.value()->setKeySequence(Config::hotkey(it.key()));
        showHotkeyStatus(it.key(), QString(), false);
    }
    showCurrentScheme();
}

void SettingsDialog::applyHotkeyScheme(Config::HotkeyScheme scheme)
{
    m_scheme->setCurrentIndex(m_scheme->findData(int(scheme)));
    emit m_scheme->activated(m_scheme->currentIndex());
}

void SettingsDialog::showHotkeyStatus(Config::Hotkey hotkey, const QString &error, bool justApplied)
{
    QLabel *status = m_hotkeyStatus.value(hotkey);
    if (!status)
        return;
    if (!error.isEmpty()) {
        status->setText(QStringLiteral("✗ ") + error);
        status->setStyleSheet("color: #d93025;");
    } else if (Config::hotkey(hotkey).isEmpty()) {
        status->setText(tr("未设置"));
        status->setStyleSheet("color: #777;");
    } else if (m_hotkeys->pausedForFullscreen()) {
        status->setText(tr("全屏时暂停"));
        status->setStyleSheet("color: #777;");
    } else if (justApplied || m_hotkeys->isRegistered(hotkey)) {
        // Just applied: change() verified the key while hotkeys were suspended for typing.
        status->setText(tr("✓ 已生效"));
        status->setStyleSheet("color: #188038;");
    } else {
        status->setText(tr("✗ 已被其他程序占用"));
        status->setStyleSheet("color: #d93025;");
    }
}

QWidget *SettingsDialog::buildSavePage()
{
    QFormLayout *form = nullptr;
    QWidget *widget = page(this, form);

    auto *ask = new QCheckBox(tr("每次保存时询问位置"), widget);
    ask->setChecked(Config::askWhereToSave());
    connect(ask, &QCheckBox::toggled, this, &Config::setAskWhereToSave);
    form->addRow(QString(), ask);
    auto *autoSave = new QCheckBox(tr("每次截图都自动存一份到保存文件夹（复制、贴图时也存）"), widget);
    autoSave->setChecked(Config::autoSaveCaptures());
    connect(autoSave, &QCheckBox::toggled, this, &Config::setAutoSaveCaptures);
    form->addRow(QString(), autoSave);
    form->addRow(QString(), hint(tr("截图时按 Ctrl+Shift+S：不询问，直接存入保存文件夹。"), widget));

    auto *dir = new QLineEdit(QDir::toNativeSeparators(Config::saveDir()), widget);
    dir->setPlaceholderText(tr("留空：使用上次保存的文件夹"));
    connect(dir, &QLineEdit::editingFinished, this,
            [dir] { Config::setSaveDir(QDir::fromNativeSeparators(dir->text().trimmed())); });
    auto *browse = new QPushButton(tr("浏览…"), widget);
    connect(browse, &QPushButton::clicked, this, [this, dir] {
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("选择保存文件夹"), Config::effectiveSaveDir());
        if (chosen.isEmpty())
            return;
        dir->setText(QDir::toNativeSeparators(chosen));
        Config::setSaveDir(chosen);
    });
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(dir, 1);
    dirRow->addWidget(browse);
    form->addRow(tr("保存文件夹："), dirRow);

    auto *format = new QComboBox(widget);
    format->addItem(QStringLiteral("PNG"), QStringLiteral("png"));
    format->addItem(QStringLiteral("JPG"), QStringLiteral("jpg"));
    format->addItem(QStringLiteral("WebP"), QStringLiteral("webp"));
    format->setCurrentIndex(qMax(0, format->findData(Config::imageFormat())));
    auto *quality = new QSpinBox(widget);
    quality->setRange(1, 100);
    quality->setSuffix(QStringLiteral(" %"));
    quality->setValue(Config::jpgQuality());
    quality->setEnabled(format->currentIndex() > 0); // PNG is lossless
    connect(format, &QComboBox::currentIndexChanged, this, [format, quality](int index) {
        Config::setImageFormat(format->itemData(index).toString());
        quality->setEnabled(index > 0);
    });
    connect(quality, &QSpinBox::valueChanged, this, &Config::setJpgQuality);
    form->addRow(tr("图片格式："), format);
    form->addRow(tr("JPG / WebP 质量："), quality);
    form->addRow(QString(), hint(tr("PNG 无损，文字最清晰；WebP 同等清晰度下文件通常比 JPG 小。"), widget));

    auto *name = new QLineEdit(Config::fileNameTemplate(), widget);
    auto *preview = hint(QString(), widget);
    auto updatePreview = [name, preview] {
        const QString pattern = name->text().trimmed().isEmpty() ? Config::defaultFileNameTemplate() : name->text();
        preview->setText(tr("示例：%1").arg(
            Config::expandFileName(pattern, Config::imageFormat(), QDateTime::currentDateTime())));
    };
    connect(name, &QLineEdit::textChanged, this, [updatePreview](const QString &text) {
        Config::setFileNameTemplate(text);
        updatePreview();
    });
    connect(format, &QComboBox::currentIndexChanged, this, updatePreview);
    updatePreview();
    form->addRow(tr("文件名："), name);
    form->addRow(QString(), preview);
    form->addRow(QString(), hint(tr("{ } 里是日期时间格式：yyyy 年、MM 月、dd 日、HH 时、mm 分、ss 秒、zzz 毫秒。"
                                    "录屏也用这个文件名。"),
                                 widget));
    return widget;
}

QWidget *SettingsDialog::buildAnnotationPage()
{
    QFormLayout *form = nullptr;
    QWidget *widget = page(this, form);

    auto *colors = new QHBoxLayout;
    colors->setSpacing(6);
    auto *group = new QButtonGroup(widget);
    for (const QColor &color : Config::annotationPalette()) {
        auto *swatch = new QToolButton(widget);
        swatch->setCheckable(true);
        swatch->setChecked(color == Config::annotationColor());
        swatch->setToolTip(color.name().toUpper());
        swatch->setFixedSize(26, 26);
        swatch->setStyleSheet(QStringLiteral("QToolButton { background: %1; border: 1px solid rgba(0,0,0,80); "
                                             "border-radius: 4px; } QToolButton:checked { border: 3px solid #1E90FF; }")
                                  .arg(color.name()));
        group->addButton(swatch);
        colors->addWidget(swatch);
        connect(swatch, &QToolButton::clicked, this, [color] { Config::setAnnotationColor(color); });
    }
    colors->addStretch();
    form->addRow(tr("默认颜色："), colors);

    auto *size = new QComboBox(widget);
    size->addItems({tr("细"), tr("中"), tr("粗")});
    size->setCurrentIndex(Config::annotationSize());
    connect(size, &QComboBox::currentIndexChanged, this, &Config::setAnnotationSize);
    form->addRow(tr("默认粗细 / 字号："), size);
    form->addRow(QString(), hint(tr("截图工具栏里改颜色和粗细时，也会记住作为下次的默认值。"), widget));
    return widget;
}

QWidget *SettingsDialog::buildTranslatePage()
{
    QFormLayout *form = nullptr;
    QWidget *widget = page(this, form);

    auto *provider = enumCombo({Translator::providerName(Config::TranslateProvider::None),
                                Translator::providerName(Config::TranslateProvider::Baidu),
                                Translator::providerName(Config::TranslateProvider::Tencent)},
                               Config::translateProvider(), widget);
    form->addRow(tr("翻译服务："), provider);

    // A saved key is never shown; typing a new one replaces it.
    auto secretField = [widget](const char *name) {
        auto *field = new QLineEdit(widget);
        field->setEchoMode(QLineEdit::Password);
        field->setPlaceholderText(Config::translateSecret(name).isEmpty() ? tr("未填写") : tr("已保存（输入新的可替换）"));
        connect(field, &QLineEdit::editingFinished, field, [field, name] {
            if (field->text().trimmed().isEmpty())
                return;
            Config::setTranslateSecret(name, field->text());
            field->clear();
            field->setPlaceholderText(tr("已保存（输入新的可替换）"));
        });
        return field;
    };
    auto link = [widget](const QString &text) {
        QLabel *label = hint(text, widget);
        label->setOpenExternalLinks(true);
        label->setTextFormat(Qt::RichText);
        return label;
    };

    auto *baiduId = new QLineEdit(Config::baiduAppId(), widget);
    connect(baiduId, &QLineEdit::editingFinished, this, [baiduId] { Config::setBaiduAppId(baiduId->text()); });
    auto *baiduKey = secretField("baiduKey");
    auto *baiduHelp = link(tr("在 <a href=\"https://fanyi-api.baidu.com/\">百度翻译开放平台</a> 注册并开通「通用文本翻译」，"
                              "在「开发者信息」里可以看到 APP ID 和密钥。"));
    form->addRow(tr("APP ID："), baiduId);
    form->addRow(tr("密钥："), baiduKey);
    form->addRow(QString(), baiduHelp);

    auto *tencentId = new QLineEdit(Config::tencentSecretId(), widget);
    connect(tencentId, &QLineEdit::editingFinished, this, [tencentId] { Config::setTencentSecretId(tencentId->text()); });
    auto *tencentKey = secretField("tencentKey");
    auto *region = new QLineEdit(Config::tencentRegion(), widget);
    connect(region, &QLineEdit::editingFinished, this, [region] { Config::setTencentRegion(region->text()); });
    auto *tencentHelp =
        link(tr("在腾讯云控制台开通「机器翻译」，然后在 <a href=\"https://console.cloud.tencent.com/cam/capi\">API 密钥管理</a> "
                "里创建 SecretId / SecretKey（建议用只授权机器翻译的子账号）。"));
    form->addRow(QStringLiteral("SecretId："), tencentId);
    form->addRow(QStringLiteral("SecretKey："), tencentKey);
    form->addRow(tr("地域："), region);
    form->addRow(QString(), tencentHelp);

    auto *test = new QPushButton(tr("测试"), widget);
    auto *testResult = hint(QString(), widget);
    auto *testRow = new QHBoxLayout;
    testRow->addWidget(test);
    testRow->addWidget(testResult, 1);
    form->addRow(QString(), testRow);
    connect(test, &QPushButton::clicked, this, [this, test, testResult] {
        test->setEnabled(false);
        testResult->setText(tr("正在连接…"));
        auto *translator = new Translator(this);
        connect(translator, &Translator::finished, this, [translator, test, testResult](const QString &text, const QString &error) {
            testResult->setText(error.isEmpty() ? tr("可以使用：Hello, world → %1").arg(text) : error);
            test->setEnabled(true);
            translator->deleteLater();
        });
        translator->translate(QStringLiteral("Hello, world"));
    });

    form->addRow(QString(), hint(tr("只有在识别窗口里点「翻译」时才会联网：识别出的文字会发送给所选的服务商。"
                                    "密钥用 Windows 账户加密保存在本机。免费额度和收费以服务商官网为准。"),
                                 widget));

    auto showFields = [form, baiduId, baiduKey, baiduHelp, tencentId, tencentKey, region, tencentHelp, test](int index) {
        const auto chosen = Config::TranslateProvider(index);
        for (QWidget *w : {static_cast<QWidget *>(baiduId), static_cast<QWidget *>(baiduKey), static_cast<QWidget *>(baiduHelp)})
            form->setRowVisible(w, chosen == Config::TranslateProvider::Baidu);
        for (QWidget *w : {static_cast<QWidget *>(tencentId), static_cast<QWidget *>(tencentKey), static_cast<QWidget *>(region),
                           static_cast<QWidget *>(tencentHelp)})
            form->setRowVisible(w, chosen == Config::TranslateProvider::Tencent);
        test->setEnabled(chosen != Config::TranslateProvider::None);
    };
    connect(provider, &QComboBox::currentIndexChanged, this, [showFields](int index) {
        Config::setTranslateProvider(Config::TranslateProvider(index));
        showFields(index);
    });
    showFields(provider->currentIndex());
    return widget;
}

QWidget *SettingsDialog::buildOcrPage()
{
    QFormLayout *form = nullptr;
    QWidget *widget = page(this, form);

    auto *engine = enumCombo({tr("自动（推荐）"), QStringLiteral("PaddleOCR"), tr("Windows 系统 OCR")},
                             Config::ocrEngine(), widget);
    connect(engine, &QComboBox::currentIndexChanged, this,
            [](int index) { Config::setOcrEngine(Config::OcrEngine(index)); });
    form->addRow(tr("识别引擎："), engine);

#ifdef JIEYING_HAVE_PADDLEOCR
    const bool paddle = PaddleOcr::isAvailable();
#else
    const bool paddle = false;
#endif
    form->addRow(QString(), hint(paddle ? tr("PaddleOCR 已安装：中英文混排、深色背景、符号识别更准，首次识别需加载约 1 秒。"
                                             "自动模式优先用它，出错时退回 Windows OCR。")
                                        : tr("PaddleOCR 未安装（缺少 onnxruntime.dll 或 ocr-models），"
                                             "所有模式都会使用 Windows 系统 OCR。"),
                                 widget));

    auto *autoCopy = new QCheckBox(tr("识别完成后自动复制文字"), widget);
    autoCopy->setChecked(Config::ocrAutoCopy());
    connect(autoCopy, &QCheckBox::toggled, this, &Config::setOcrAutoCopy);
    form->addRow(QString(), autoCopy);
    return widget;
}

QWidget *SettingsDialog::buildRecordingPage()
{
    QFormLayout *form = nullptr;
    QWidget *widget = page(this, form);

    auto *gifFps = new QSpinBox(widget);
    gifFps->setRange(5, 30);
    gifFps->setSuffix(tr(" 帧/秒"));
    gifFps->setValue(Config::gifFps());
    connect(gifFps, &QSpinBox::valueChanged, this, &Config::setGifFps);
    form->addRow(tr("GIF 帧率："), gifFps);

    auto *mp4Fps = new QSpinBox(widget);
    mp4Fps->setRange(10, 60);
    mp4Fps->setSuffix(tr(" 帧/秒"));
    mp4Fps->setValue(Config::mp4Fps());
    connect(mp4Fps, &QSpinBox::valueChanged, this, &Config::setMp4Fps);
    form->addRow(tr("MP4 帧率："), mp4Fps);

    auto *quality = enumCombo({tr("低（文件小）"), tr("中（推荐）"), tr("高（文字最清晰）")}, Config::mp4Quality(), widget);
    connect(quality, &QComboBox::currentIndexChanged, this,
            [](int index) { Config::setMp4Quality(Config::VideoQuality(index)); });
    form->addRow(tr("MP4 画质："), quality);

    auto *systemAudio = new QCheckBox(tr("录制系统声音（电脑播放的声音）"), widget);
    systemAudio->setChecked(Config::recordSystemAudio());
    connect(systemAudio, &QCheckBox::toggled, this, &Config::setRecordSystemAudio);
    form->addRow(tr("MP4 声音："), systemAudio);
    auto *microphone = new QCheckBox(tr("录制麦克风"), widget);
    microphone->setChecked(Config::recordMicrophone());
    connect(microphone, &QCheckBox::toggled, this, &Config::setRecordMicrophone);
    form->addRow(QString(), microphone);
    form->addRow(QString(), hint(tr("录制时也可以用录屏面板上的喇叭、麦克风按钮随时开关。GIF 没有声音。"), widget));

    auto *cursor = new QCheckBox(tr("录入鼠标指针"), widget);
    cursor->setChecked(Config::recordCursor());
    connect(cursor, &QCheckBox::toggled, this, &Config::setRecordCursor);
    form->addRow(QString(), cursor);
    auto *clicks = new QCheckBox(tr("显示鼠标点击（左键蓝圈、右键橙圈）"), widget);
    clicks->setChecked(Config::recordClicks());
    connect(clicks, &QCheckBox::toggled, this, &Config::setRecordClicks);
    form->addRow(QString(), clicks);
    auto *keys = new QCheckBox(tr("显示按下的快捷键（只显示组合键和功能键，不显示输入的文字）"), widget);
    keys->setChecked(Config::recordKeys());
    connect(keys, &QCheckBox::toggled, this, &Config::setRecordKeys);
    form->addRow(QString(), keys);
    auto *backend = enumCombo({tr("自动（推荐）"), QStringLiteral("Windows.Graphics.Capture"), QStringLiteral("GDI")},
                              Config::captureBackend(), widget);
    connect(backend, &QComboBox::currentIndexChanged, this,
            [](int index) { Config::setCaptureBackend(Config::CaptureBackend(index)); });
    form->addRow(tr("采集方式："), backend);
    form->addRow(QString(), hint(WgcGrabber::canHideBorder()
                                     ? tr("自动：使用 Windows.Graphics.Capture（显卡合成画面，大区域、高帧率更流畅）。")
                                     : tr("自动：区域较小时用 GDI；区域大到 GDI 跟不上帧率时改用 Windows.Graphics.Capture。"
                                          "在 Windows 10 上它会在屏幕四周显示黄色边框，这是系统行为，无法关闭。"),
                                 widget));
    form->addRow(QString(), hint(tr("GIF 帧率越高越流畅，文件也越大；屏幕录制一般 10–15 帧足够。"), widget));
    return widget;
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (qobject_cast<QKeySequenceEdit *>(watched)) {
        if (event->type() == QEvent::FocusIn)
            m_hotkeys->setSuspended(true);
        else if (event->type() == QEvent::FocusOut)
            m_hotkeys->setSuspended(false);
    }
    return QDialog::eventFilter(watched, event);
}

QString SettingsDialog::schemeShown() const
{
    return m_scheme->currentText();
}
