#!/usr/bin/env bash
# Publish a built firmware into dist/ with a sequential, sortable name.
#
# Naming: dist/Casper-v0.2.0.NNNN.bin
#   Product version comes from [casper] version in platformio.ini (the leading
#   "v" is kept). NNNN is a zero-padded counter kept in dist/.build-number, so
#   `ls` always lists builds oldest -> newest.
#
# Usage:
#   scripts/dist_bin.sh [env]        # env defaults to gh_release
#
# Example:
#   scripts/dist_bin.sh gh_release   -> dist/Casper-v0.2.0.0000.bin
set -euo pipefail

ENV_NAME="${1:-gh_release}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO_ROOT/.pio/build/$ENV_NAME/firmware.bin"
DIST="$REPO_ROOT/dist"
COUNTER="$DIST/.build-number"

if [[ ! -f "$SRC" ]]; then
  echo "dist_bin: no firmware at $SRC (build '$ENV_NAME' first)" >&2
  exit 1
fi

mkdir -p "$DIST"
[[ -f "$COUNTER" ]] || echo 0 > "$COUNTER"

NEXT=$(( $(cat "$COUNTER") + 1 ))
echo "$NEXT" > "$COUNTER"

VERSION="$(
  python3 - "$REPO_ROOT" <<'PY'
import configparser, sys
config = configparser.ConfigParser()
config.read(sys.argv[1] + "/platformio.ini")
print(config.get("casper", "version", fallback="v0.0.0").strip())
PY
)"
# [casper] version is already "v0.2.0"
OUT="$DIST/$(printf 'Casper-%s.%04d.bin' "$VERSION" "$NEXT")"

cp -f "$SRC" "$OUT"
echo "$OUT"
