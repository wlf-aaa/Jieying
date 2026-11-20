// ONNX Runtime first, before Qt's macros; its headers are not /W4 clean.
// Manual API init: by default the header calls into onnxruntime.dll from a static initializer,
// which would defeat the delay-load and stop the app from starting when the DLL is absent.
#define ORT_API_MANUAL_INIT
#pragma warning(push, 0)
#include <onnxruntime_cxx_api.h>
#pragma warning(pop)

#include "paddleocr.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

// PaddleOCR's defaults for DB post-processing and recognition.
constexpr float kBinaryThreshold = 0.3f;
constexpr float kBoxThreshold = 0.6f;
constexpr double kUnclipRatio = 1.5;
constexpr float kDropScore = 0.5f;
constexpr int kRecHeight = 48;
constexpr int kRecMinWidth = 320;
constexpr int kRecMaxWidth = 3200;
// Detection input: small captures are enlarged so text is big enough, huge ones are capped.
constexpr int kDetUpscaleTo = 960;
constexpr int kDetMaxSide = 2560;

const char *const kModelFiles[] = {"det.onnx", "rec.onnx", "dict.txt"};

struct Model
{
    std::unique_ptr<Ort::Session> session;
    std::string input;
    std::string output;
};

struct Engine
{
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "jieying"};
    Model det;
    Model rec;
    QStringList dictionary; // model class i (1-based; 0 is the CTC blank) is dictionary[i - 1]
};

Model loadModel(Ort::Env &env, const QString &path)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(qMax(1, QThread::idealThreadCount() / 2));
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();

    Model model;
    model.session = std::make_unique<Ort::Session>(env, nativePath.c_str(), options);
    Ort::AllocatorWithDefaultOptions allocator;
    model.input = model.session->GetInputNameAllocated(0, allocator).get();
    model.output = model.session->GetOutputNameAllocated(0, allocator).get();
    return model;
}

// Loaded once on first use; Ort::Session::Run is safe to call from several threads.
Engine &engine()
{
    static std::once_flag once;
    static std::unique_ptr<Engine> instance;
    static std::string error;
    std::call_once(once, [] {
        try {
            Ort::InitApi(); // first call into onnxruntime.dll; callers checked isAvailable()
            auto loaded = std::make_unique<Engine>();
            const QDir dir(PaddleOcr::modelDir());
            loaded->det = loadModel(loaded->env, dir.filePath("det.onnx"));
            loaded->rec = loadModel(loaded->env, dir.filePath("rec.onnx"));
            QFile file(dir.filePath("dict.txt"));
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("cannot read dict.txt");
            QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
            for (QString &line : lines) {
                if (line.endsWith(QLatin1Char('\r')))
                    line.chop(1);
            }
            while (!lines.isEmpty() && lines.last().isEmpty())
                lines.removeLast();
            loaded->dictionary = lines;
            instance = std::move(loaded);
        } catch (const std::exception &e) {
            error = e.what();
        }
    });
    if (!instance)
        throw std::runtime_error("PaddleOCR unavailable: " + error);
    return *instance;
}

std::vector<float> run(Model &model, std::vector<float> &input, const std::array<int64_t, 4> &shape,
                       std::vector<int64_t> &outputShape)
{
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
    const char *inputs[] = {model.input.c_str()};
    const char *outputs[] = {model.output.c_str()};
    auto result = model.session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    outputShape = result[0].GetTensorTypeAndShapeInfo().GetShape();
    size_t count = 1;
    for (int64_t dim : outputShape)
        count *= size_t(dim);
    const float *data = result[0].GetTensorData<float>();
    return std::vector<float>(data, data + count);
}

// NCHW float tensor, zero-padded on the right to `width`. PaddleOCR models were trained on
// OpenCV images, so channels go in B, G, R order.
std::vector<float> toTensor(const QImage &rgb, const std::array<float, 3> &mean, const std::array<float, 3> &stdev,
                            int width)
{
    const int height = rgb.height();
    std::vector<float> tensor(size_t(3) * height * width, 0.f);
    for (int y = 0; y < height; ++y) {
        const uchar *row = rgb.constScanLine(y);
        for (int x = 0; x < rgb.width(); ++x) {
            const float bgr[3] = {row[3 * x + 2] / 255.f, row[3 * x + 1] / 255.f, row[3 * x] / 255.f};
            for (int c = 0; c < 3; ++c)
                tensor[(size_t(c) * height + y) * width + x] = (bgr[c] - mean[c]) / stdev[c];
        }
    }
    return tensor;
}

QList<QRect> detect(Engine &engine, const QImage &rgb)
{
    const int side = qMax(rgb.width(), rgb.height());
    double scale = 1.0;
    if (side < kDetUpscaleTo)
        scale = qMin(4.0, double(kDetUpscaleTo) / side);
    else if (side > kDetMaxSide)
        scale = double(kDetMaxSide) / side;
    auto multipleOf32 = [](double v) { return qMax(32, int(std::lround(v / 32)) * 32); };
    const int width = multipleOf32(rgb.width() * scale);
    const int height = multipleOf32(rgb.height() * scale);
    const QImage resized = rgb.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    std::vector<float> input = toTensor(resized, {0.485f, 0.456f, 0.406f}, {0.229f, 0.224f, 0.225f}, width);
    std::vector<int64_t> shape;
    const std::vector<float> probability = run(engine.det, input, {1, 3, height, width}, shape);
    const int mapHeight = int(shape[2]);
    const int mapWidth = int(shape[3]);
    const double sx = double(mapWidth) / rgb.width();
    const double sy = double(mapHeight) / rgb.height();

    // Connected regions of the thresholded probability map are text kernels; each is scored,
    // then grown back to the full text extent ("unclip") as DBNet shrinks them during training.
    QList<QRect> boxes;
    std::vector<uchar> visited(size_t(mapWidth) * mapHeight, 0);
    std::vector<int> stack;
    for (int start = 0; start < mapWidth * mapHeight; ++start) {
        if (visited[start] || probability[start] <= kBinaryThreshold)
            continue;
        int left = mapWidth, right = -1, top = mapHeight, bottom = -1, count = 0;
        double sum = 0;
        stack.assign(1, start);
        visited[start] = 1;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const int x = i % mapWidth;
            const int y = i / mapWidth;
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, y);
            bottom = qMax(bottom, y);
            sum += probability[i];
            ++count;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= mapWidth || ny >= mapHeight)
                        continue;
                    const int n = ny * mapWidth + nx;
                    if (!visited[n] && probability[n] > kBinaryThreshold) {
                        visited[n] = 1;
                        stack.push_back(n);
                    }
                }
            }
        }

        const int w = right - left + 1;
        const int h = bottom - top + 1;
        if (qMin(w, h) < 3 || sum / count < kBoxThreshold)
            continue;
        const double grow = double(w) * h * kUnclipRatio / (2.0 * (w + h));
        const QRectF grown(left - grow, top - grow, w + 2 * grow, h + 2 * grow);
        const QRect box = QRectF(grown.x() / sx, grown.y() / sy, grown.width() / sx, grown.height() / sy)
                              .toAlignedRect()
                              .intersected(rgb.rect());
        if (box.width() >= 3 && box.height() >= 3)
            boxes << box;
    }
    return boxes;
}

bool isLatin(QChar c)
{
    return c.unicode() < 0x2E80; // everything before the CJK radical/ideograph blocks
}

// Which columns of the line image hold no glyph pixels, and how tall the glyphs are, judged
// against the background (average of the top and bottom rows, which the detector pads).
struct Ink
{
    std::vector<bool> blankColumn;
    int height = 0;
};

Ink measureInk(const QImage &line)
{
    auto luminance = [](const uchar *p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; };
    int background = 0;
    for (int x = 0; x < line.width(); ++x)
        background += luminance(line.constScanLine(0) + 3 * x) + luminance(line.constScanLine(line.height() - 1) + 3 * x);
    background /= 2 * line.width();

    Ink ink;
    ink.blankColumn.assign(size_t(line.width()), true);
    int top = line.height(), bottom = -1;
    for (int y = 0; y < line.height(); ++y) {
        const uchar *row = line.constScanLine(y);
        for (int x = 0; x < line.width(); ++x) {
            if (qAbs(luminance(row + 3 * x) - background) > 48) {
                ink.blankColumn[size_t(x)] = false;
                top = qMin(top, y);
                bottom = y;
            }
        }
    }
    ink.height = qMax(0, bottom - top + 1);
    return ink;
}

std::pair<QString, float> recognizeLine(Engine &engine, const QImage &crop)
{
    const int width = qBound(8, int(std::ceil(kRecHeight * double(crop.width()) / crop.height())), kRecMaxWidth);
    const QImage resized = crop.scaled(width, kRecHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int padded = qMax(width, kRecMinWidth);
    std::vector<float> input = toTensor(resized, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, padded);
    std::vector<int64_t> shape;
    const std::vector<float> output = run(engine.rec, input, {1, 3, kRecHeight, padded}, shape);
    const int steps = int(shape[1]);
    const int classes = int(shape[2]);
    if (classes != engine.dictionary.size() + 2) // blank + dictionary + trailing space class
        throw std::runtime_error("dictionary does not match the recognition model");

    // Greedy CTC decoding: best class per step, collapse repeats, drop blanks. Each character
    // remembers the x (in the resized line) of the step that emitted it.
    struct Glyph
    {
        QString text;
        double x;
    };
    std::vector<Glyph> glyphs;
    float confidence = 0;
    int previous = 0;
    for (int t = 0; t < steps; ++t) {
        const float *scores = output.data() + size_t(t) * classes;
        const int best = int(std::max_element(scores, scores + classes) - scores);
        if (best != 0 && best != previous) {
            const QString text = best - 1 < engine.dictionary.size() ? engine.dictionary[best - 1] : QStringLiteral(" ");
            glyphs.push_back({text, (t + 0.5) * padded / steps});
            confidence += scores[best];
        }
        previous = best;
    }

    // The Chinese-trained model never emits a space between Chinese and Latin text. Restore one
    // where the image shows a real gap: a run of blank columns at least a fifth of the glyph
    // height wide between the two characters (gaps inside glyphs are narrower).
    constexpr double kSpaceGap = 0.2;
    const Ink ink = measureInk(resized);
    QString text;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        if (i > 0 && !text.endsWith(QLatin1Char(' ')) && glyphs[i].text != QLatin1String(" ")
            && isLatin(glyphs[i - 1].text.back()) != isLatin(glyphs[i].text.front())) {
            int run = 0, longest = 0;
            for (int x = int(glyphs[i - 1].x); x < qMin(int(glyphs[i].x), resized.width()); ++x) {
                run = ink.blankColumn[size_t(x)] ? run + 1 : 0;
                longest = qMax(longest, run);
            }
            if (longest >= kSpaceGap * ink.height)
                text += QLatin1Char(' ');
        }
        text += glyphs[i].text;
    }
    return {text, glyphs.empty() ? 0.f : confidence / float(glyphs.size())};
}

} // namespace

QString PaddleOcr::modelDir()
{
    const QString overridden = qEnvironmentVariable("JIEYING_OCR_MODELS"); // for tests/ocr_eval
    if (!overridden.isEmpty())
        return overridden;
    return QDir(QCoreApplication::applicationDirPath()).filePath("ocr-models");
}

bool PaddleOcr::isAvailable()
{
    if (!QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath("onnxruntime.dll")))
        return false;
    const QDir dir(modelDir());
    for (const char *file : kModelFiles) {
        if (!QFileInfo::exists(dir.filePath(QLatin1String(file))))
            return false;
    }
    return true;
}

void PaddleOcr::warmUp()
{
    try {
        engine();
    } catch (const std::exception &) {
        // recognize() reports the failure and falls back when OCR is actually used
    }
}

OcrResult PaddleOcr::recognize(const QImage &image)
{
    Engine &e = engine();
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);

    struct Piece
    {
        QRect box;
        QString text;
    };
    std::vector<Piece> pieces;
    for (const QRect &box : detect(e, rgb)) {
        const auto [text, confidence] = recognizeLine(e, rgb.copy(box));
        if (confidence >= kDropScore && !text.trimmed().isEmpty())
            pieces.push_back({box, text.trimmed()});
    }

    // Reading order: pieces that overlap vertically by half their height share a row;
    // rows go top to bottom, pieces within a row left to right.
    std::sort(pieces.begin(), pieces.end(), [](const Piece &a, const Piece &b) { return a.box.top() < b.box.top(); });
    struct Row
    {
        QRect box;
        std::vector<Piece> pieces;
    };
    std::vector<Row> rows;
    for (const Piece &piece : pieces) {
        auto sameRow = [&piece](const Row &row) {
            const int overlap = qMin(row.box.bottom(), piece.box.bottom()) - qMax(row.box.top(), piece.box.top());
            return overlap * 2 >= qMin(row.box.height(), piece.box.height());
        };
        auto row = std::find_if(rows.begin(), rows.end(), sameRow);
        if (row == rows.end()) {
            rows.push_back({piece.box, {piece}});
        } else {
            row->box |= piece.box;
            row->pieces.push_back(piece);
        }
    }

    OcrResult result;
    result.language = QStringLiteral("PaddleOCR PP-OCRv5");
    for (Row &row : rows) {
        std::sort(row.pieces.begin(), row.pieces.end(),
                  [](const Piece &a, const Piece &b) { return a.box.left() < b.box.left(); });
        QStringList texts;
        for (const Piece &piece : row.pieces)
            texts << piece.text;
        result.lines.append({texts.join(QLatin1Char(' ')), row.box});
    }
    return result;
}
