#!/usr/bin/env bash
# Publish a built firmware into dist/ with a sequential, sortable name.
#
# Naming: dist/CrossPoint-<NNNN>-<shortsha>.bin
#   NNNN is a zero-padded counter kept in dist/.build-number, so `ls` always
#   lists builds oldest -> newest. The short SHA keeps each build traceable to
#   a commit without having to remember which hash was which.
#
# Usage:
#   scripts/dist_bin.sh [env]        # env defaults to gh_release
#
# Example:
#   scripts/dist_bin.sh gh_release   -> dist/CrossPoint-0007-79219c65.bin
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

SHA="$(cd "$REPO_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo nogit)"
OUT="$DIST/$(printf 'CrossPoint-%04d-%s.bin' "$NEXT" "$SHA")"

cp -f "$SRC" "$OUT"
echo "$OUT"
