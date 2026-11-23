#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "  OctoHost -> GitHub Release Publisher"
echo "=========================================="
echo ""
read -r -p "Paste your GitHub repository URL (e.g. https://github.com/username/OctoHost.git): " REPO_URL
if [ -z "$REPO_URL" ]; then
    echo "Error: Repository URL cannot be empty."
    exit 1
fi

read -r -p "Release version tag [v3.1.0]: " VERSION_TAG
VERSION_TAG="${VERSION_TAG:-v3.1.0}"

if [ ! -d ".git" ]; then
    echo "==> Initializing Git repository..."
    git init -b main
fi

echo "==> Staging source and workflow files (build/ is ignored)..."
git add .

if ! git diff --cached --quiet; then
    git commit -m "Release ${VERSION_TAG}: OctoHost macOS & Windows VST3/AU Workstation"
else
    echo "==> No new changes to commit."
fi

if git remote get-url origin >/dev/null 2>&1; then
    git remote set-url origin "$REPO_URL"
else
    git remote add origin "$REPO_URL"
fi

echo "==> Pushing main branch..."
git push -u origin main

echo "==> Tagging ${VERSION_TAG} and triggering Mac + Windows cloud builds..."
git tag -f "$VERSION_TAG"
git push -f origin "$VERSION_TAG"

echo ""
echo "Done! Open the 'Actions' tab on GitHub to watch macOS Universal & Windows x64 compile."
echo "When finished, both ZIPs will appear automatically under 'Releases'."
