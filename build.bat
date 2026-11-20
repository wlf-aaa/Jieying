@echo off
rem Configure and build with MSVC + Ninja. Usage: build.bat [Debug|Release]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

cmake -S "%~dp0." -B "%~dp0build\%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_PREFIX_PATH=D:/Qt/6.10.3/msvc2022_64 || exit /b 1
cmake --build "%~dp0build\%CONFIG%" || exit /b 1
