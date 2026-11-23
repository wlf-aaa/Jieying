@echo off
rem Builds and runs the headless test suites (Qt offscreen platform; nothing appears on screen).
rem Usage: run_tests.bat [output-dir-for-rendered-images]
setlocal
call "%~dp0build.bat" Debug || exit /b 1
set PATH=D:\Qt\6.10.3\msvc2022_64\bin;%PATH%
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0build\test-output
if not exist "%OUT%" mkdir "%OUT%"

set FAILED=0
for %%t in (overlay_test ocr_test m4_test settings_test) do (
    echo === %%t
    "%~dp0build\Debug\%%t.exe" "%OUT%" 2>nul || set FAILED=1
)
for %%t in (historydialog_test collage_test tableocr_test livepin_test) do (
    echo === %%t
    "%~dp0build\Debug\%%t.exe" 2>nul || set FAILED=1
)
if %FAILED%==1 (echo SOME TESTS FAILED & exit /b 1)
echo ALL TESTS PASSED
