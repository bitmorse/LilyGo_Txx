#!/usr/bin/env bash
# One-time installer for ESP-IDF, pinned to a stable release.
# Idempotent: safe to run again; skips the clone if it already exists.
set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
IDF_BRANCH="${IDF_BRANCH:-release/v5.3}"

echo "==> ESP-IDF target dir : $IDF_PATH"
echo "==> ESP-IDF branch     : $IDF_BRANCH"

if [ ! -d "$IDF_PATH/.git" ]; then
    echo "==> Cloning ESP-IDF (this downloads a few hundred MB)..."
    mkdir -p "$(dirname "$IDF_PATH")"
    git clone -b "$IDF_BRANCH" --depth 1 --recursive \
        https://github.com/espressif/esp-idf.git "$IDF_PATH"
else
    echo "==> ESP-IDF already cloned, skipping git clone."
fi

echo "==> Installing the esp32 toolchain (this is the big ~2 GB step)..."
"$IDF_PATH/install.sh" esp32

echo
echo "==> Done. You're ready to build:"
echo "      make flash-monitor"
