#!/usr/bin/env python3
"""8×8 checker JPEGs for ProgressiveCoverJpegTest."""

from pathlib import Path

from PIL import Image

W, H, CELL = 128, 192, 8
OUT = Path(__file__).resolve().parent / "fixtures"
OUT.mkdir(parents=True, exist_ok=True)


def checker_rgb() -> Image.Image:
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            dark = ((x // CELL) + (y // CELL)) % 2 == 0
            v = 16 if dark else 240
            px[x, y] = (v, v, v)
    return im


def main() -> None:
    rgb = checker_rgb()
    gray = rgb.convert("L")
    rgb.save(OUT / "cover_progressive.jpg", quality=95, progressive=True, subsampling=2, optimize=False)
    gray.save(OUT / "cover_progressive_gray.jpg", quality=95, progressive=True, optimize=False)
    gray.save(OUT / "cover_baseline_gray.jpg", quality=95, progressive=False, optimize=False)
    print("wrote", *sorted(p.name for p in OUT.glob("*.jpg")))


if __name__ == "__main__":
    main()
