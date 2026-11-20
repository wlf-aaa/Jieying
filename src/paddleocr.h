#pragma once

#include "ocr.h"

// PP-OCRv5 (detection + recognition) on ONNX Runtime. The runtime DLL and the models
// (ocr-models/det.onnx, rec.onnx, dict.txt) are looked up next to the executable.
namespace PaddleOcr {

QString modelDir();

// True when the runtime and all model files are present. onnxruntime.dll is delay-loaded,
// so nothing may call into it unless this returns true.
bool isAvailable();

// Blocking; run it on a worker thread. Throws std::exception if the engine cannot run.
OcrResult recognize(const QImage &image);

// Loads the models now (about a second) so the first recognize() is fast. Blocking; never throws.
void warmUp();

} // namespace PaddleOcr
