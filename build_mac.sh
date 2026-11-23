#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v cmake >/dev/null 2>&1; then
    echo "Error: CMake is not installed. Install via Homebrew: brew install cmake"
    exit 1
fi

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Error: Xcode Command Line Tools not found. Run: xcode-select --install"
    exit 1
fi

echo "==> Configuring OctoHost (Release)..."
cmake -B build -DCMAKE_BUILD_TYPE=Release

echo "==> Building OctoHost.app..."
CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build build --config Release -j "$CORES"

APP_PATH="build/OctoHost_artefacts/Release/OctoHost.app"
if [ ! -d "$APP_PATH" ]; then
    APP_PATH="build/OctoHost_artefacts/OctoHost.app"
fi

echo "==> Build complete: $APP_PATH"
open "$APP_PATH"
