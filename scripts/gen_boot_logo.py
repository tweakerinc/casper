#!/usr/bin/env python3
"""
Optional: lightly smooth the *original* CrossPoint sheet-ghost Logo120.

Default product logo is git-tracked src/images/Logo120.h (do not redesign).
This script only softens stair-step edges; it must not flood-fill the white body.

Usage:
  python scripts/gen_boot_logo.py          # write previews only
  python scripts/gen_boot_logo.py --write  # also overwrite Logo120.h (review first)
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "images"
SS = 6


def load_original_bytes() -> bytes:
    raw = subprocess.check_output(["git", "show", "HEAD:src/images/Logo120.h"], cwd=ROOT)
    text = raw.decode("utf-8", errors="replace")
    start = text.index("Logo120[]")
    end = text.index("};", start)
    hexes = re.findall(r"0x([0-9a-fA-F]{2})", text[start:end])
    data = bytes(int(h, 16) for h in hexes)
    if len(data) != 1800:
        raise SystemExit(f"HEAD Logo120.h expected 1800 bytes, got {len(data)}")
    return data


def unpack_msb(data: bytes, size: int = 120) -> Image.Image:
    row_bytes = size // 8
    arr = np.full((size, size), 255, dtype=np.uint8)
    for y in range(size):
        for x in range(size):
            b = data[y * row_bytes + (x >> 3)]
            bit = 7 - (x & 7)
            if ((b >> bit) & 1) == 0:
                arr[y, x] = 0
    return Image.fromarray(arr, mode="L")


def pack_msb(img_l: Image.Image) -> bytes:
    w, h = img_l.size
    px = img_l.load()
    row_bytes = w // 8
    out = bytearray([0xFF] * (row_bytes * h))
    for y in range(h):
        for x in range(w):
            if px[x, y] < 128:
                out[y * row_bytes + (x >> 3)] &= ~(1 << (7 - (x & 7)))
    return bytes(out)


def edge_smooth(upright: Image.Image) -> Image.Image:
    """Soften only the outline stair-steps; never fill white interior/folds."""
    size = upright.size[0]
    ref = np.array(upright.convert("L"))
    hi = upright.resize((size * SS, size * SS), Image.Resampling.NEAREST)
    hi = hi.filter(ImageFilter.GaussianBlur(radius=SS * 0.4))
    sm = np.array(hi.resize((size, size), Image.Resampling.LANCZOS))
    smooth_ink = sm < 128
    ink_ref = ref < 128

    ref_img = Image.fromarray(ref, mode="L")
    # thin edge band only
    band = (np.array(ref_img.filter(ImageFilter.MinFilter(3))) < 128) != (
        np.array(ref_img.filter(ImageFilter.MaxFilter(3))) < 128
    )

    out_ink = ink_ref.copy()
    out_ink[band] = smooth_ink[band]
    # hard clamp: cannot add ink more than 1px from original black
    near = np.array(ref_img.filter(ImageFilter.MinFilter(3))) < 128
    out_ink &= near | ink_ref
    # hard clamp: cannot remove original black (no broken outline)
    out_ink |= ink_ref

    out = np.full((size, size), 255, dtype=np.uint8)
    out[out_ink] = 0
    return Image.fromarray(out, mode="L")


def write_header(path: Path, data: bytes) -> None:
    lines = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// 'casper', 120x120px 1-bit (0=black, 1=white).",
        "// Original sheet-ghost; light edge smooth only. Pre-rotated 90° CCW for portrait drawImage.",
        "static const uint8_t Logo120[] = {",
    ]
    parts: list[str] = []
    for i, b in enumerate(data):
        parts.append(f"0x{b:02x}")
        if (i + 1) % 16 == 0:
            lines.append("    " + ", ".join(parts) + ",")
            parts = []
    if parts:
        lines.append("    " + ", ".join(parts) + ",")
    lines.append("};")
    lines.append("")
    lines.append('static_assert(sizeof(Logo120) == 1800, "Logo120 must be exactly 120x120 / 8 bytes");')
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="Overwrite Logo120.h (default: preview only)")
    args = ap.parse_args()

    packed = unpack_msb(load_original_bytes(), 120)
    upright = packed.transpose(Image.Transpose.ROTATE_270)
    upright.convert("1").save(OUT / "casper_orig_preview.png")

    smooth = edge_smooth(upright)
    smooth.convert("1").save(OUT / "casper_smooth_preview.png")

    if args.write:
        data = pack_msb(smooth.transpose(Image.Transpose.ROTATE_90))
        write_header(OUT / "Logo120.h", data)
        smooth.convert("1").save(OUT / "casper.png")
        packed_out = smooth.transpose(Image.Transpose.ROTATE_90)
        packed_out.convert("1").save(OUT / "Logo120.png")
        print("Wrote Logo120.h + pngs (edge-smoothed original)")
    else:
        print("Preview only: casper_orig_preview.png / casper_smooth_preview.png")
        print("Pass --write after visual check to replace Logo120.h")


if __name__ == "__main__":
    main()
