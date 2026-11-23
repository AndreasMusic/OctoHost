@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo Error: CMake is not installed or not in PATH.
    pause
    exit /b 1
)

echo ==> Configuring OctoHost (Windows x64 Release)...
cmake -B build -A x64
if errorlevel 1 exit /b 1

echo ==> Building OctoHost.exe...
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

echo ==> Build complete: build\OctoHost_artefacts\Release\OctoHost.exe
start "" "build\OctoHost_artefacts\Release\OctoHost.exe"
