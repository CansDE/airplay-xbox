@echo off
rem Baut die App fuer x64-UWP (Xbox). Aufruf: tools\build.bat [configure|build]
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 uwp 10.0.26100.0 || exit /b 1
set "CMAKE_DIR=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%CMAKE_DIR%\CMake\bin;%CMAKE_DIR%\Ninja;%PATH%"
cd /d "%~dp0.."
if "%1"=="configure" goto configure
if "%1"=="" goto both
goto build
:both
:configure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0 || exit /b 1
if "%1"=="configure" exit /b 0
:build
cmake --build build %2 %3 %4 || exit /b 1
