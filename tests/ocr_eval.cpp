// OCR accuracy measurement (not part of run_tests): renders UI-like Chinese/English text with
// GDI ClearType into memory bitmaps, as real screenshots look, recognizes it with PaddleOCR
// and reports the character error rate per font, size and theme.
// Usage: ocr_eval [output-dir]   (JIEYING_OCR_MODELS=<dir>: other models; JIEYING_OCR_RAW=1: no clean-up)
#include "paddleocr.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>

#include <cstdio>

#include <windows.h>

namespace {

const QStringList kCorpus = {
    QStringLiteral("文件已保存到 D:\\Documents\\报告_2026年10月.docx"),
    QStringLiteral("请在下方输入验证码，有效期为5分钟。"),
    QStringLiteral("系统检测到您的账号在其他设备登录，如非本人操作请及时修改密码。"),
    QStringLiteral("会议时间：10月30日（周四）下午 14:30–16:00，地点：3楼大会议室"),
    QStringLiteral("下载速度 12.5 MB/s，剩余时间约 3 分 20 秒"),
    QStringLiteral("我们已收到您的订单，预计明天送达。"),
    QStringLiteral("点击「设置」→「隐私与安全」→「权限管理」即可修改。"),
    QStringLiteral("The quick brown fox jumps over the lazy dog."),
    QStringLiteral("Error 0x80070005: Access is denied. (HRESULT)"),
    QStringLiteral("npm install --save-dev typescript@5.4.2"),
    QStringLiteral("本次更新修复了若干已知问题，并优化了启动速度。"),
    QStringLiteral("温馨提示：天气转凉，注意添衣保暖！"),
    QStringLiteral("总计：¥1,286.50（含运费 ¥12.00）"),
    QStringLiteral("张三、李四、王五已加入群聊“项目讨论组”"),
    QStringLiteral("如有疑问，请拨打客服电话 400-820-8820 或发送邮件至 support@example.com"),
    QStringLiteral("截影是一款免费的截图、贴图、文字识别工具。"),
    QStringLiteral("第一章 概述 1.1 背景与目标"),
    QStringLiteral("复制成功！已复制 128 个字符到剪贴板"),
    QStringLiteral("上一页 下一页 共 25 页 跳转至 第 页"),
    QStringLiteral("用户名或密码错误，您还有 2 次尝试机会"),
    QStringLiteral("Windows 安全中心 病毒和威胁防护 已启用"),
    QStringLiteral("深度学习模型在推理阶段的延迟受批大小与线程数影响。"),
    QStringLiteral("鬓、龘、齉、爨、彧、婧、玥、喆、堃、淼"),
    QStringLiteral("已读 23:05  对方正在输入…"),
};

struct Page
{
    QImage image;
    QStringList lines;
};

// GDI ClearType text in a memory DIB: nothing is drawn on screen.
Page renderPage(const QString &font, int pixelSize, bool dark)
{
    const int lineHeight = int(pixelSize * 1.9);
    const int width = 1000;
    const int height = 16 * 2 + lineHeight * int(kCorpus.size());

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    const COLORREF background = dark ? RGB(30, 30, 30) : RGB(255, 255, 255);
    const COLORREF ink = dark ? RGB(212, 212, 212) : RGB(32, 32, 32);
    RECT all{0, 0, width, height};
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &all, brush);
    DeleteObject(brush);

    HFONT hfont = CreateFontW(-pixelSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, reinterpret_cast<LPCWSTR>(font.utf16()));
    HGDIOBJ oldFont = SelectObject(dc, hfont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    for (int i = 0; i < kCorpus.size(); ++i) {
        const std::wstring text = kCorpus[i].toStdWString();
        TextOutW(dc, 16, 16 + i * lineHeight, text.c_str(), int(text.size()));
    }
    GdiFlush();

    QImage image(static_cast<const uchar *>(bits), width, height, width * 4, QImage::Format_RGB32);
    Page page{image.copy(), kCorpus};
    SelectObject(dc, oldFont);
    DeleteObject(hfont);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return page;
}

int editDistance(const QString &a, const QString &b)
{
    QList<int> row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            row[j] = qMin(qMin(row[j] + 1, row[j - 1] + 1), diagonal + (a[i - 1] == b[j - 1] ? 0 : 1));
            diagonal = above;
        }
    }
    return row[b.size()];
}

QString squeeze(QString s)
{
    s.remove(QLatin1Char(' '));
    s.remove(QLatin1Char('\n'));
    return s;
}

struct Score
{
    int errors = 0;
    int chars = 0;
    qint64 ms = 0;
};

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication::addLibraryPath(QStringLiteral(JIEYING_QT_PLUGIN_DIR));
    QApplication app(argc, argv);
    const QDir out(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath());
    if (!PaddleOcr::isAvailable()) {
        std::printf("PaddleOCR not available (models: %s)\n", qPrintable(PaddleOcr::modelDir()));
        return 1;
    }
    PaddleOcr::warmUp();

    const bool raw = qEnvironmentVariableIsSet("JIEYING_OCR_RAW");
    const QStringList fonts = {QStringLiteral("Microsoft YaHei"), QStringLiteral("SimSun")};
    const int sizes[] = {12, 13, 14, 16, 20};

    std::printf("%-10s", "");
    for (const QString &font : fonts)
        for (int size : sizes)
            std::printf(" %s%-2d", font == QLatin1String("SimSun") ? "S" : "Y", size);
    std::printf("   light  dark  total    time\n");

    Score light, dark;
    std::printf("%-10s", raw ? "raw" : "pipeline");
    for (const QString &font : fonts) {
        for (int size : sizes) {
            Score cell;
            for (bool isDark : {false, true}) {
                const Page page = renderPage(font, size, isDark);
                QElapsedTimer timer;
                timer.start();
                const OcrResult result =
                    raw ? PaddleOcr::recognize(page.image) : Ocr::recognize(page.image, Config::OcrEngine::Paddle);
                const qint64 ms = timer.elapsed();
                const QString truth = squeeze(page.lines.join(QString()));
                const QString got = squeeze(result.text());
                const int errors = editDistance(truth, got);
                Score &theme = isDark ? dark : light;
                theme.errors += errors;
                theme.chars += int(truth.size());
                theme.ms += ms;
                cell.errors += errors;
                cell.chars += int(truth.size());
                if (size == 14 && !isDark) { // a sample of what goes wrong
                    page.image.save(out.filePath(QStringLiteral("eval_%1_%2.png").arg(font.left(1)).arg(size)));
                    QStringList wrong;
                    const QList<OcrLine> lines = result.lines;
                    for (int i = 0; i < page.lines.size(); ++i) {
                        const QString expected = squeeze(page.lines[i]);
                        const QString seen = i < lines.size() ? squeeze(lines[i].text) : QString();
                        if (expected != seen)
                            wrong << QStringLiteral("    - %1\n    + %2").arg(expected, seen);
                    }
                    std::fprintf(stderr, "[%s %dpx light] lines differing:\n%s\n", qPrintable(font), size,
                                 wrong.join(QLatin1Char('\n')).toUtf8().constData());
                }
            }
            std::printf(" %5.1f", 100.0 * cell.errors / cell.chars);
        }
    }
    const double total = 100.0 * (light.errors + dark.errors) / (light.chars + dark.chars);
    std::printf("   %5.2f %5.2f %6.2f %6lldms\n", 100.0 * light.errors / light.chars, 100.0 * dark.errors / dark.chars,
                total, (light.ms + dark.ms) / 20);
    std::printf("(character error rate %%, spaces ignored; time = average per page)\n");
    return 0;
}
