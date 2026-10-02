#!/usr/bin/env python3
"""Rebuild Casper built-in Literata headers (stock FreeType, no stem-cal / gap-fill).

Nominal ppem @150 DPI only — same density as pre-cpfont-editor Casper/CrossPoint.
Line spacing Tight/Normal/Wide stays in CrossPointSettings::getReaderLineCompression().

Requires: freetype-py, fonttools (same as fontconvert.py)
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
OUT_DIR = SCRIPT_DIR.parent / "builtinFonts"
SRC = OUT_DIR / "source" / "Literata"
FONTCONVERT = SCRIPT_DIR / "fontconvert.py"

STYLES = {
    "regular": "Literata-Regular.ttf",
    "italic": "Literata-Italic.ttf",
    "bold": "Literata-Bold.ttf",
    "bolditalic": "Literata-BoldItalic.ttf",
}

SIZES = [10, 12, 14, 16]


def main() -> int:
    for style, ttf_name in STYLES.items():
        ttf = SRC / ttf_name
        if not ttf.is_file():
            print(f"missing source TTF: {ttf}", file=sys.stderr)
            return 1

    for size in SIZES:
        for style, ttf_name in STYLES.items():
            name = f"literata_{size}_{style}"
            out = OUT_DIR / f"{name}.h"
            ttf = SRC / ttf_name
            # Stock convert + CrossInk/YACP --darken-aa (darker edge greys).
            # No stem-cal --ppem, no --gap-fill (those made glyphs thin/harsh).
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
            if r.stderr:
                for line in r.stderr.strip().splitlines()[-3:]:
                    print(f"     {line}", flush=True)
    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
