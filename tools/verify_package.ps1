# Verifies the packages in dist\ the way a user would get them, without starting the app's UI:
#   1. installer: silent per-user install into a scratch folder -> self-check -> uninstall entry and
#      Start menu shortcut present -> silent uninstall -> files, uninstall entry, autostart and
#      settings all gone
#   2. portable zip: extract -> self-check -> settings stored in data\ beside the exe
#   3. portable copy without ONNX Runtime / OCR models -> self-check passes on Windows OCR
#   -Lite: extract the lite zip, verify Windows OCR, add the OCR pack, verify PaddleOCR
# Usage: powershell -ExecutionPolicy Bypass -File tools\verify_package.ps1 [-Lite] [-PackageDir <path>]
param([switch]$Lite, [string]$PackageDir)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$dist = if ($PackageDir) { (Resolve-Path -LiteralPath $PackageDir).Path } else { Join-Path $root 'dist' }
$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(Jieying VERSION ([\d.]+)').Groups[1].Value
$work = Join-Path $env:TEMP "jieying-verify-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$failures = 0
function Check($ok, $what) {
    Write-Host ('{0}  {1}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what)
    if (-not $ok) { $script:failures++ }
}
# Keep codec probe files in the verification folder and keep Qt and the build tree off PATH.
function SelfCheck($exe, $report) {
    $savedPath = $env:Path
    $savedTemp = $env:TEMP
    $savedTmp = $env:TMP
    $probeDir = Split-Path -Parent $report
    $env:Path = "$env:SystemRoot\system32;$env:SystemRoot"
    $env:TEMP = $probeDir
    $env:TMP = $probeDir
    try {
        $p = Start-Process $exe -ArgumentList '--self-check', "`"$report`"" -WindowStyle Hidden -Wait -PassThru
    } finally {
        $env:Path = $savedPath
        $env:TEMP = $savedTemp
        $env:TMP = $savedTmp
    }
    $text = Get-Content $report -Encoding utf8 -ErrorAction SilentlyContinue
    return @{ Ok = ($p.ExitCode -eq 0); Text = $text }
}

if ($Lite) {
    Write-Host '== lite portable zip'
    $appDir = Join-Path $work 'lite\Jieying'
    $portableExe = Join-Path $appDir 'jieying.exe'
    Expand-Archive -LiteralPath (Join-Path $dist "Jieying-$version-lite-portable.zip") -DestinationPath (Join-Path $work 'lite')
    Check ((-not (Test-Path (Join-Path $appDir 'onnxruntime.dll'))) -and
           (-not (Test-Path (Join-Path $appDir 'ocr-models')))) 'lite zip excludes the OCR runtime and models'
    $check = SelfCheck $portableExe (Join-Path $work 'lite-check.txt')
    $check.Text | Where-Object { $_ -match 'OCR|settings:|^FAIL' } | ForEach-Object { Write-Host "      $_" }
    Check $check.Ok 'lite copy passes the self-check'
    Check ($check.Text -match '^PASS  default OCR engine reads the sample.*Windows OCR') 'lite defaults to Windows OCR'
    Check ($check.Text -like "INFO  settings:*$appDir\data\*.ini") 'portable settings live in data\ beside the exe'

    Write-Host '== optional PaddleOCR pack'
    $exeHash = (Get-FileHash $portableExe).Hash
    Expand-Archive -LiteralPath (Join-Path $dist "Jieying-$version-paddleocr.zip") -DestinationPath $appDir -Force
    Check ((Get-FileHash $portableExe).Hash -eq $exeHash) 'the pack leaves the lite executable intact'
    $check = SelfCheck $portableExe (Join-Path $work 'lite-upgraded-check.txt')
    $check.Text | Where-Object { $_ -match 'OCR|^FAIL' } | ForEach-Object { Write-Host "      $_" }
    Check $check.Ok 'lite copy with the OCR pack passes the self-check'
    Check ($check.Text -match '^PASS  default OCR engine reads the sample.*PaddleOCR') 'lite defaults to PaddleOCR with the pack installed'
    $workItem = Get-Item -LiteralPath $work
    if ($workItem.Parent.FullName -ne (Resolve-Path -LiteralPath $env:TEMP).Path -or
        ($workItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'unexpected verification folder' }
    Remove-Item -LiteralPath $workItem.FullName -Recurse -Force
    exit $failures
}

$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{98CAFD72-1599-4CC3-ACD8-C780D7B68A43}_is1'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$settingsKey = 'HKCU:\Software\Jieying'
if (Test-Path $settingsKey) { throw "$settingsKey exists (real settings?); uninstall would delete it — aborting" }
if ((Get-ItemProperty $runKey).PSObject.Properties.Name -contains 'Jieying') { throw 'a real Jieying autostart entry exists — aborting' }
$historyDir = Join-Path $env:LOCALAPPDATA 'Jieying'
if (Test-Path $historyDir) { throw "$historyDir exists (real capture history?); uninstall would delete it — aborting" }

# ---- 1. installer -------------------------------------------------------------------------------
Write-Host '== installer'
$appDir = Join-Path $work 'installed'
$setup = Join-Path $dist "Jieying-$version-setup.exe"
$p = Start-Process $setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/DIR=`"$appDir`"",
    '/TASKS="autostart"', "/LOG=`"$work\install.log`"" -Wait -PassThru
Check ($p.ExitCode -eq 0 -and (Test-Path "$appDir\jieying.exe")) 'silent install succeeds'
$check = SelfCheck "$appDir\jieying.exe" "$work\installed-check.txt"
$check.Text | ForEach-Object { Write-Host "      $_" }
Check $check.Ok 'installed copy passes the self-check'
$entry = Get-ItemProperty $uninstallKey -ErrorAction SilentlyContinue
Check ($entry -and $entry.DisplayName -eq '截影' -and $entry.DisplayVersion -eq $version) 'uninstall entry: 截影 with the version'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) '截影.lnk'
Check (Test-Path $shortcut) 'Start menu shortcut created'
$run = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).Jieying
Check ($run -eq "`"$appDir\jieying.exe`" --autostart") 'autostart task writes the same entry as the app'
New-Item -Force $settingsKey | Out-Null # stands in for settings the app would have saved
New-Item -ItemType Directory -Force "$historyDir\Jieying\history" | Out-Null # stands in for capture history
Set-Content "$historyDir\Jieying\history\probe.png" 'x'
Set-ItemProperty $settingsKey -Name probe -Value 1

$uninstaller = Get-ChildItem $appDir -Filter 'unins*.exe' | Select-Object -First 1
$p = Start-Process $uninstaller.FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
Start-Sleep -Seconds 2 # the uninstaller finishes removing itself from a temp copy
Check ($p.ExitCode -eq 0 -and -not (Test-Path "$appDir\jieying.exe")) 'silent uninstall removes the program'
Check (-not (Test-Path $uninstallKey) -and -not (Test-Path $shortcut)) 'uninstall entry and shortcut removed'
Check (-not ((Get-ItemProperty $runKey).PSObject.Properties.Name -contains 'Jieying')) 'autostart entry removed'
Check (-not (Test-Path $settingsKey)) 'settings removed'
Check (-not (Test-Path $historyDir)) 'capture history removed'
Remove-Item -Recurse -Force $settingsKey -ErrorAction SilentlyContinue

# ---- 2. portable --------------------------------------------------------------------------------
Write-Host '== portable zip'
Expand-Archive (Join-Path $dist "Jieying-$version-portable.zip") -DestinationPath "$work\portable"
$portableExe = "$work\portable\Jieying\jieying.exe"
$check = SelfCheck $portableExe "$work\portable-check.txt"
Check $check.Ok 'portable copy passes the self-check'
$settingsLine = $check.Text | Where-Object { $_ -like 'INFO  settings:*' }
Write-Host "      $settingsLine"
Check ($settingsLine -like "*$work\portable\Jieying\data\*.ini") 'portable settings live in data\ beside the exe'

# ---- 3. without PaddleOCR -----------------------------------------------------------------------
Write-Host '== without ONNX Runtime / OCR models'
Copy-Item -Recurse "$work\portable\Jieying" "$work\no-paddle"
Remove-Item "$work\no-paddle\onnxruntime.dll", "$work\no-paddle\ocr-models" -Recurse
$check = SelfCheck "$work\no-paddle\jieying.exe" "$work\no-paddle-check.txt"
$check.Text | Where-Object { $_ -match 'OCR' } | ForEach-Object { Write-Host "      $_" }
Check ($check.Ok -and ($check.Text -match 'default OCR engine reads the sample.*Windows OCR')) 'falls back to Windows OCR and still passes'

Remove-Item -Recurse -Force $work
Write-Host $(if ($failures) { "FAILED ($failures)" } else { 'ALL PACKAGE CHECKS PASSED' })
exit $failures
