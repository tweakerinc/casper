#!/usr/bin/env python3
"""Rebuild built-in UI/reader headers from Sourcerer TTFs (Source Serif 4 fork).

Sourcerer: https://github.com/nicoverbruggen/sourcerer (OFL)
  thicker Source Serif variant tailored for e-readers.

Firmware still uses sourceserif4_* header names and SOURCESERIF4_* font IDs
so settings enums / ladders stay stable. User-facing label is "Sourcerer".

  8 regular          — status / hints / small chrome
  10 regular+bold    — recents titles / menus
  12/14/16/18 R/I/B/BI — reader + large chrome
  72 clock           — rebuild with gen_clock_font.py (Sourcerer Bold)

Requires: freetype-py, fonttools; TTFs in builtinFonts/source/Sourcerer/
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
OUT_DIR = SCRIPT_DIR.parent / "builtinFonts"
SRC = OUT_DIR / "source" / "Sourcerer"
FONTCONVERT = SCRIPT_DIR / "fontconvert.py"

# Sourcerer v1.4 release names (Sourcerer.zip)
STYLES = {
    "regular": "Sourcerer-Regular.ttf",
    "italic": "Sourcerer-Italic.ttf",
    "bold": "Sourcerer-Bold.ttf",
    "bolditalic": "Sourcerer-BoldItalic.ttf",
}

SIZES_STYLES: dict[int, list[str]] = {
    8: ["regular"],
    10: ["regular", "bold"],
    12: ["regular", "italic", "bold", "bolditalic"],
    14: ["regular", "italic", "bold", "bolditalic"],
    16: ["regular", "italic", "bold", "bolditalic"],
    18: ["regular", "italic", "bold", "bolditalic"],
}


def main() -> int:
    for style, ttf_name in STYLES.items():
        ttf = SRC / ttf_name
        if not ttf.is_file():
            print(f"missing source TTF: {ttf}", file=sys.stderr)
            print("Download Sourcerer.zip from https://github.com/nicoverbruggen/sourcerer/releases", file=sys.stderr)
            return 1

    for size, styles in SIZES_STYLES.items():
        for style in styles:
            # Keep historical header names so fontIds / all.h / insertFont stay put.
            name = f"sourceserif4_{size}_{style}"
            out = OUT_DIR / f"{name}.h"
            ttf = SRC / STYLES[style]
            cmd = [
                sys.executable,
                str(FONTCONVERT),
                name,
                str(size),
                str(ttf),
                "--2bit",
                "--compress",
                "--pnum",
                "--darken-aa",
            ]
            print(" ".join(cmd), flush=True)
            with out.open("w", encoding="utf-8", newline="\n") as f:
                r = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE, text=True)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                return r.returncode
            print(f"  -> {out.name} ({out.stat().st_size} bytes)", flush=True)
    print("done (run gen_clock_font.py for 72_clock from Sourcerer-Bold)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
