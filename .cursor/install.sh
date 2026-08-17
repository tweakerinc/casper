#!/usr/bin/env bash
# Cloud Agent install script for the Casper / CrossPoint Reader firmware.
# Idempotent: safe to re-run. Runs from the repository root after checkout.
set -euo pipefail

# 1. System packages the ESP32 build + host tooling need on top of the base image.
#    (apt is a no-op when they are already present.)
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  python3-venv \
  build-essential \
  cmake \
  clang-format \
  cppcheck \
  ca-certificates

# 2. Vendored FreeInk SDK (and its nested Lucide icon submodule) live in a git
#    submodule; a fresh checkout leaves it empty and the firmware will not link.
git submodule update --init --recursive

# 3. Python virtualenv with the pinned pioarduino PlatformIO Core fork plus the
#    project's build-script dependencies (Pillow, cairosvg, fonttools, ...).
if [ ! -x .venv/bin/pio ]; then
  python3 -m venv .venv
fi
# shellcheck disable=SC1091
source .venv/bin/activate
python -m pip install --quiet --upgrade pip
python -m pip install --quiet \
  "https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip" \
  -r requirements.txt

# 4. Pre-fetch the ESP32-C3 platform, toolchain and libraries so the first
#    `pio run` does not stall on a multi-hundred-MB download.
pio pkg install -e default

echo "Casper dev environment ready. Activate with: source .venv/bin/activate"
