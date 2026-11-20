# Downloads third-party dependencies into third_party/ and verifies their SHA-256:
#   - ONNX Runtime (CPU, MIT) from Microsoft's GitHub release
#   - PP-OCRv5 mobile detection/recognition ONNX models + dictionary (Apache-2.0),
#     as converted by the RapidOCR project, from ModelScope
#   - ZXing-cpp (Apache-2.0) source, for QR code / barcode reading
# Re-running skips files that are already present and verified.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$third = Join-Path $root 'third_party'
$ortVersion = '1.31.1'
$modelBase = 'https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/master'
$zxingVersion = '3.1.1'

$downloads = @(
    @{ Url = "https://github.com/microsoft/onnxruntime/releases/download/v$ortVersion/onnxruntime-win-x64-$ortVersion.zip"
       File = "downloads/onnxruntime-win-x64-$ortVersion.zip"
       Sha256 = '5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c' },
    @{ Url = "$modelBase/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_mobile.onnx"
       File = 'ocr-models/det.onnx'
       Sha256 = '4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae' },
    @{ Url = "$modelBase/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile.onnx"
       File = 'ocr-models/rec.onnx'
       Sha256 = '5825fc7ebf84ae7a412be049820b4d86d77620f204a041697b0494669b1742c5' },
    @{ Url = "$modelBase/paddle/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile/ppocrv5_dict.txt"
       File = 'ocr-models/dict.txt'
       Sha256 = 'd1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b' },
    @{ Url = "https://github.com/zxing-cpp/zxing-cpp/releases/download/v$zxingVersion/zxing-cpp-$zxingVersion.zip"
       File = "downloads/zxing-cpp-$zxingVersion.zip"
       Sha256 = 'c914877d2598e6c748725e9bc0dad9b667710537c6b91a3d8b8a7214834ee1d5' }
)

function Test-Hash($path, $expected) {
    (Test-Path $path) -and ((Get-FileHash $path -Algorithm SHA256).Hash.ToLower() -eq $expected)
}

foreach ($d in $downloads) {
    $target = Join-Path $third $d.File
    if (Test-Hash $target $d.Sha256) { Write-Host "ok        $($d.File)"; continue }
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Write-Host "download  $($d.File)"
    Invoke-WebRequest $d.Url -OutFile "$target.part" -UseBasicParsing
    if (-not (Test-Hash "$target.part" $d.Sha256)) {
        Remove-Item "$target.part"
        throw "SHA-256 mismatch for $($d.Url)"
    }
    Move-Item -Force "$target.part" $target
}

$ortDir = Join-Path $third 'onnxruntime'
if (-not (Test-Path (Join-Path $ortDir 'lib/onnxruntime.dll'))) {
    $zip = Join-Path $third "downloads/onnxruntime-win-x64-$ortVersion.zip"
    $tmp = Join-Path $third 'downloads/ort-extract'
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    Expand-Archive $zip -DestinationPath $tmp
    Remove-Item -Recurse -Force $ortDir -ErrorAction SilentlyContinue
    Move-Item (Join-Path $tmp "onnxruntime-win-x64-$ortVersion") $ortDir
    Remove-Item -Recurse -Force $tmp
    Write-Host "extracted onnxruntime $ortVersion"
}
$zxingDir = Join-Path $third 'zxing-cpp'
if (-not (Test-Path (Join-Path $zxingDir 'CMakeLists.txt'))) {
    $zip = Join-Path $third "downloads/zxing-cpp-$zxingVersion.zip"
    $tmp = Join-Path $third 'downloads/zxing-extract'
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    Expand-Archive $zip -DestinationPath $tmp
    Remove-Item -Recurse -Force $zxingDir -ErrorAction SilentlyContinue
    Move-Item (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName $zxingDir
    Remove-Item -Recurse -Force $tmp
    Write-Host "extracted zxing-cpp $zxingVersion"
}
Write-Host 'Dependencies ready.'
