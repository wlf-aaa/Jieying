# Builds the release packages into dist\:
#   Jieying-<version>-setup.exe     Inno Setup installer
#   Jieying-<version>-portable.zip  unzip-and-run copy; keeps settings in data\ next to the exe
# -Lite builds two ZIPs instead of the full releases:
#   Jieying-<version>-lite-portable.zip  Windows OCR by default
#   Jieying-<version>-paddleocr.zip      optional runtime and models, extracted beside jieying.exe
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Lite] [-SkipBuild] [-OutputDir <path>]
param([switch]$SkipBuild, [switch]$Lite, [string]$OutputDir)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$dist = if ($OutputDir) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir) }
        else { Join-Path $root 'dist' }
$buildConfig = if ($Lite) { 'Release-Lite' } else { 'Release' }
$build = Join-Path $root "build\$buildConfig"

$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(Jieying VERSION ([\d.]+)').Groups[1].Value
if (-not $version) { throw 'version not found in CMakeLists.txt' }
Write-Host "截影 $version ($buildConfig)"

$ortDll = Join-Path $root 'third_party\onnxruntime\lib\onnxruntime.dll'
$models = @('det.onnx', 'rec.onnx', 'dict.txt') | ForEach-Object { Join-Path $root "third_party\ocr-models\$_" }
$missingOcr = @(@($ortDll) + $models | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
if ($Lite -and $missingOcr.Count) {
    throw 'PaddleOCR files are required to build the optional pack. Run tools\fetch_deps.ps1.'
}

if (-not $SkipBuild) {
    & cmd /d /c "`"$root\build.bat`" $buildConfig"
    if ($LASTEXITCODE -ne 0) { throw "$buildConfig build failed" }
}

# Qt and compiler locations from the build's CMake cache / the Visual Studio install.
$cache = Get-Content (Join-Path $build 'CMakeCache.txt')
$qtDir = ($cache | Select-String '^Qt6_DIR:PATH=(.+)$').Matches[0].Groups[1].Value
$windeployqt = Join-Path $qtDir '..\..\..\bin\windeployqt.exe'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT') -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $Lite) {
    $iscc = @($env:JIEYING_ISCC, 'D:\Exinstall\InnoSetup7\ISCC.exe', "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $iscc) { throw 'ISCC.exe (Inno Setup) not found; set JIEYING_ISCC' }
}

# ---- stage ------------------------------------------------------------------------------------
$stageName = if ($Lite) { 'stage-lite' } else { 'stage' }
$stageRoot = Join-Path $dist $stageName
if (Test-Path -LiteralPath $stageRoot) {
    $existingStage = Get-Item -LiteralPath $stageRoot -Force
    if ($existingStage.Parent.FullName -ne (Resolve-Path -LiteralPath $dist).Path -or
        ($existingStage.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unexpected staging folder: $stageRoot"
    }
    Remove-Item -LiteralPath $existingStage.FullName -Recurse -Force
}
$stage = Join-Path $stageRoot 'Jieying'
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'jieying.exe') $stage
& $windeployqt --release --translations zh_CN --no-compiler-runtime --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler `
    --verbose 0 (Join-Path $stage 'jieying.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }

$ocrStage = if ($Lite) { Join-Path $stageRoot 'PaddleOCR' } else { $stage }
if (-not $missingOcr.Count) {
    $ocrModels = Join-Path $ocrStage 'ocr-models'
    New-Item -ItemType Directory -Force $ocrModels | Out-Null
    Copy-Item -LiteralPath $ortDll $ocrStage
    Copy-Item -LiteralPath $models $ocrModels
} else {
    Write-Warning 'PaddleOCR files missing (run tools\fetch_deps.ps1): package will use Windows OCR only'
}
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
$notices = Join-Path $root 'licenses'
Copy-Item -LiteralPath $notices -Destination $stage -Recurse
$license = Join-Path $root 'LICENSE'
if (Test-Path -LiteralPath $license) { Copy-Item -LiteralPath $license $stage }
if ($Lite) {
    $ocrLicenseDir = Join-Path $ocrStage 'licenses'
    New-Item -ItemType Directory -Force $ocrLicenseDir | Out-Null
    $ocrNotices = @('Apache-2.0.txt', 'onnxruntime-LICENSE.txt', 'onnxruntime-ThirdPartyNotices.txt', 'THIRD_PARTY_NOTICES.txt')
    $ocrNotices | ForEach-Object { Copy-Item -LiteralPath (Join-Path $notices $_) $ocrLicenseDir }
}

# ---- self-check the staged copy, with a PATH that cannot reach Qt or the build folder ----------
$report = Join-Path $stageRoot 'self-check.txt'
$savedPath = $env:Path
$savedTemp = $env:TEMP
$savedTmp = $env:TMP
$env:Path = "$env:SystemRoot\system32;$env:SystemRoot"
$env:TEMP = $stageRoot
$env:TMP = $stageRoot
try {
    $p = Start-Process (Join-Path $stage 'jieying.exe') -ArgumentList '--self-check', "`"$report`"" -WindowStyle Hidden -Wait -PassThru
} finally {
    $env:Path = $savedPath
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
}
$checks = Get-Content -LiteralPath $report -Encoding utf8
$checks | ForEach-Object { Write-Host "  $_" }
if ($p.ExitCode -ne 0) { throw 'self-check of the staged app failed' }
if ($Lite) {
    if (-not ($checks -match '^PASS  default OCR engine reads the sample.*Windows OCR') -or
        -not ($checks -match '^INFO  PaddleOCR not installed')) {
        throw 'the lite app must use Windows OCR and support the optional PaddleOCR pack'
    }
} elseif (-not $missingOcr.Count -and -not ($checks -match '^PASS  default OCR engine reads the sample.*PaddleOCR')) {
    throw 'the full app did not use the bundled PaddleOCR runtime and models'
}

# ---- portable packages -------------------------------------------------------------------------
function New-PackageZip([string[]]$source, [string]$name) {
    $archive = Join-Path $stageRoot $name
    Compress-Archive -LiteralPath $source -DestinationPath $archive -CompressionLevel Optimal
    Move-Item -LiteralPath $archive -Destination (Join-Path $dist $name) -Force
}

$portableRoot = Join-Path $stageRoot 'portable'
$portable = Join-Path $portableRoot 'Jieying'
New-Item -ItemType Directory -Force $portableRoot | Out-Null
Copy-Item -LiteralPath $stage -Destination $portable -Recurse
Set-Content -LiteralPath (Join-Path $portable 'jieying.portable') -Encoding utf8 -Value @(
    '便携模式：有这个文件时，截影把设置保存在旁边的 data 文件夹，而不是注册表。',
    'Portable mode: while this file exists, settings are kept in the data folder next to it.')
if ($Lite) {
    $files = @("Jieying-$version-lite-portable.zip", "Jieying-$version-paddleocr.zip")
    New-PackageZip $portable $files[0]
    New-PackageZip (Get-ChildItem -LiteralPath $ocrStage).FullName $files[1]
} else {
    $files = @("Jieying-$version-portable.zip", "Jieying-$version-setup.exe")
    New-PackageZip $portable $files[0]
    & $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/O$dist" (Join-Path $root 'installer\jieying.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compile failed' }
}

Write-Host ''
foreach ($file in $files) {
    $item = Get-Item (Join-Path $dist $file)
    $hash = (Get-FileHash $item.FullName -Algorithm SHA256).Hash.ToLower()
    Write-Host ('{0,-34} {1,6:N1} MB  sha256 {2}' -f $file, ($item.Length / 1MB), $hash)
}
