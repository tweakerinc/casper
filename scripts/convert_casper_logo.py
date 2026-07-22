#!/usr/bin/env python3
"""Convert casperbootlogo.png into src/images/Logo120.h (120x120 1-bit).

Boot drawImage is pure 1-bit (no gray AA on panel). We still refine the art
with a supersample → soft blur → Lanczos downscale → threshold pipeline so
edges stair-step less than a raw 1-bit convert.

GfxRenderer::drawImage transforms coordinates for portrait but does not rotate
bitmap bits; bake a 90° CCW pre-rotation so the logo stands upright on device.
"""

from pathlib import Path

from PIL import Image, ImageFilter, ImageOps

ROOT = Path(__file__).resolve().parents[1]
SRC = Path(r"C:\Users\m\Downloads\casperbootlogo.png")
DST_PNG = ROOT / "src" / "images" / "casper.png"
DST_PREVIEW = ROOT / "src" / "images" / "casper_preview.png"
DST_H = ROOT / "src" / "images" / "Logo120.h"

OUT_SIZE = 120
# Work at 4x so edge smoothing survives the final resize.
SUPER = 4
PRE_ROTATE = Image.Transpose.ROTATE_90
# Slightly above mid-gray keeps the silhouette solid without growing sparse fringe.
THRESHOLD = 148


def load_grayscale(path: Path) -> Image.Image:
    """RGBA → white background, grayscale."""
    im = Image.open(path)
    if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
        rgba = im.convert("RGBA")
        bg = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
        return Image.alpha_composite(bg, rgba).convert("L")
    return im.convert("L")


def refine_to_1bit(gray: Image.Image) -> Image.Image:
    """Supersample + mild blur + high-quality downscale + threshold.

    True multi-level AA cannot ship in the boot 1-bit path; this approximates
    smoother curves by filtering before the final binary cut.
    """
    # Normalize contrast so soft gray art still becomes a clean silhouette.
    gray = ImageOps.autocontrast(gray, cutoff=0.5)

    hi = gray.resize((OUT_SIZE * SUPER, OUT_SIZE * SUPER), Image.Resampling.LANCZOS)
    # Soften jaggies before the final shrink (edge AA proxy).
    hi = hi.filter(ImageFilter.GaussianBlur(radius=0.9))
    # Mild unsharp after blur keeps features (eyes / mouth) defined.
    hi = hi.filter(ImageFilter.UnsharpMask(radius=1.2, percent=80, threshold=2))

    lo = hi.resize((OUT_SIZE, OUT_SIZE), Image.Resampling.LANCZOS)
    # Final 1-bit cut for e-ink.
    bw = lo.point(lambda p: 255 if p >= THRESHOLD else 0, mode="1")
    return bw


def pack_bitmap(im: Image.Image) -> bytearray:
    w, h = im.size
    assert w == OUT_SIZE and h == OUT_SIZE
    pixels = im.load()
    row_bytes = w // 8
    data = bytearray()
    for y in range(h):
        for bx in range(row_bytes):
            byte = 0
            for bit in range(8):
                x = bx * 8 + bit
                # 0xff = white, 0x00 = black in e-ink framebuffer
                if pixels[x, y] != 0:
                    byte |= 0x80 >> bit
            data.append(byte)
    assert len(data) == OUT_SIZE * OUT_SIZE // 8
    return data


def write_header(data: bytearray) -> None:
    lines = [
        "#pragma once",
        "#include <cstdint>",
        "",
        f"// 'casper', {OUT_SIZE}x{OUT_SIZE}px 1-bit (0=black, 1=white).",
        "// Refined via supersample/blur/downscale; pre-rotated 90° CCW for portrait drawImage.",
        "static const uint8_t Logo120[] = {",
    ]
    chunk = 19
    for i in range(0, len(data), chunk):
        part = data[i : i + chunk]
        hexes = ", ".join(f"0x{b:02x}" for b in part)
        comma = "," if i + chunk < len(data) else ""
        lines.append(f"    {hexes}{comma}")
    lines.append("};")
    lines.append("")
    lines.append(
        f'static_assert(sizeof(Logo120) == {len(data)}, "Logo120 must be exactly {OUT_SIZE}x{OUT_SIZE} / 8 bytes");'
    )
    lines.append("")
    DST_H.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> None:
    gray = load_grayscale(SRC)
    bw = refine_to_1bit(gray)
    # Bake panel orientation after refinement so filtering stays upright in source space.
    bw = bw.transpose(PRE_ROTATE)

    bw.save(DST_PNG)
    # Preview without device rotation (easier to check on PC).
    refine_to_1bit(gray).convert("L").save(DST_PREVIEW)

    data = pack_bitmap(bw)
    write_header(data)
    black = sum(1 for b in data for i in range(8) if not ((b >> (7 - i)) & 1))
    print(f"wrote {DST_H} ({len(data)} bytes, {black} black pixels)")
    print(f"preview (upright): {DST_PREVIEW}")
    print("note: boot path is 1-bit only; refinement smooths stair-steps, not true gray AA")


if __name__ == "__main__":
    main()
