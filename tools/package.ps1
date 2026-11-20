# Builds the release packages into dist\:
#   Jieying-<version>-setup.exe     Inno Setup installer
#   Jieying-<version>-portable.zip  unzip-and-run copy; keeps settings in data\ next to the exe
# Steps: Release build -> clean staging folder (windeployqt, ONNX Runtime + OCR models, VC++
# runtime, licenses) -> self-check of the staged app -> zip -> installer.
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-SkipBuild]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$build = Join-Path $root 'build\Release'

$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(Jieying VERSION ([\d.]+)').Groups[1].Value
if (-not $version) { throw 'version not found in CMakeLists.txt' }
Write-Host "截影 $version"

if (-not $SkipBuild) {
    & cmd /c "`"$root\build.bat`" Release"
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
}

# Qt and compiler locations from the build's CMake cache / the Visual Studio install.
$cache = Get-Content (Join-Path $build 'CMakeCache.txt')
$qtDir = ($cache | Select-String '^Qt6_DIR:PATH=(.+)$').Matches[0].Groups[1].Value
$windeployqt = Join-Path $qtDir '..\..\..\bin\windeployqt.exe'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT') -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
$iscc = @($env:JIEYING_ISCC, 'D:\Exinstall\InnoSetup7\ISCC.exe', "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $iscc) { throw 'ISCC.exe (Inno Setup) not found; set JIEYING_ISCC' }

# ---- stage ------------------------------------------------------------------------------------
$stage = Join-Path $dist 'stage\Jieying'
Remove-Item -Recurse -Force (Join-Path $dist 'stage') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $build 'jieying.exe') $stage
& $windeployqt --release --translations zh_CN --no-compiler-runtime --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler `
    --verbose 0 (Join-Path $stage 'jieying.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }

$ortDll = Join-Path $root 'third_party\onnxruntime\lib\onnxruntime.dll'
$models = Join-Path $root 'third_party\ocr-models'
if ((Test-Path $ortDll) -and (Test-Path $models)) {
    Copy-Item $ortDll $stage
    New-Item -ItemType Directory -Force (Join-Path $stage 'ocr-models') | Out-Null
    Copy-Item (Join-Path $models '*') (Join-Path $stage 'ocr-models')
} else {
    Write-Warning 'PaddleOCR files missing (run tools\fetch_deps.ps1): package will use Windows OCR only'
}
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null
Copy-Item (Join-Path $root 'licenses\*') (Join-Path $stage 'licenses')

# ---- self-check the staged copy, with a PATH that cannot reach Qt or the build folder ----------
$report = Join-Path $dist 'stage\self-check.txt'
$savedPath = $env:Path
$env:Path = "$env:SystemRoot\system32;$env:SystemRoot"
try {
    $p = Start-Process (Join-Path $stage 'jieying.exe') -ArgumentList '--self-check', "`"$report`"" -Wait -PassThru
} finally { $env:Path = $savedPath }
Get-Content $report -Encoding utf8 | ForEach-Object { Write-Host "  $_" }
if ($p.ExitCode -ne 0) { throw 'self-check of the staged app failed' }

# ---- portable zip -------------------------------------------------------------------------------
$portableRoot = Join-Path $dist 'stage\portable'
$portable = Join-Path $portableRoot 'Jieying'
New-Item -ItemType Directory -Force $portableRoot | Out-Null
Copy-Item -Recurse $stage $portable
Set-Content (Join-Path $portable 'jieying.portable') -Encoding utf8 -Value @(
    '便携模式：有这个文件时，截影把设置保存在旁边的 data 文件夹，而不是注册表。',
    'Portable mode: while this file exists, settings are kept in the data folder next to it.')
$zip = Join-Path $dist "Jieying-$version-portable.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $portable -DestinationPath $zip -CompressionLevel Optimal

# ---- installer ----------------------------------------------------------------------------------
& $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/O$dist" (Join-Path $root 'installer\jieying.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compile failed' }

Write-Host ''
foreach ($file in @("Jieying-$version-setup.exe", "Jieying-$version-portable.zip")) {
    $item = Get-Item (Join-Path $dist $file)
    $hash = (Get-FileHash $item.FullName -Algorithm SHA256).Hash.ToLower()
    Write-Host ('{0,-34} {1,6:N1} MB  sha256 {2}' -f $file, ($item.Length / 1MB), $hash)
}
