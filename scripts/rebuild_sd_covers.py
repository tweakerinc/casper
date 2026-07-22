#!/usr/bin/env python3
"""Extract EPUB covers and write dashboard thumbs onto the SD card."""

from __future__ import annotations

import io
import re
import struct
import zipfile
from pathlib import Path

from PIL import Image

ROOT = Path(r"I:\.crosspoint")
BOOKS = [
    (
        "/Fantasy/Dungeon Crawler Carl/01 - Dungeon Crawler Carl.epub",
        Path(r"I:\Fantasy\Dungeon Crawler Carl\01 - Dungeon Crawler Carl.epub"),
    ),
    (
        "/Fantasy/Dungeon Crawler Carl/02 - Carl's Doomsday Scenario.epub",
        Path(r"I:\Fantasy\Dungeon Crawler Carl\02 - Carl's Doomsday Scenario.epub"),
    ),
    (
        "/Fantasy/Dungeon Crawler Carl/03 - The Dungeon Anarchist's Cookbook.epub",
        Path(r"I:\Fantasy\Dungeon Crawler Carl\03 - The Dungeon Anarchist's Cookbook.epub"),
    ),
    (
        "/Fantasy/Dungeon Crawler Carl/04 - The Gate of the Feral Gods.epub",
        Path(r"I:\Fantasy\Dungeon Crawler Carl\04 - The Gate of the Feral Gods.epub"),
    ),
]

# (width, height, fit/contain)
SIZES = [
    (296, 444, True),   # dashboard adaptive
    (297, 445, False),
    (123, 180, False),
]


def fnv1a64(s: str) -> int:
    h = 14695981039346656037
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def find_cover_bytes(zf: zipfile.ZipFile) -> bytes | None:
    names = zf.namelist()
    opf = next((n for n in names if n.lower().endswith(".opf")), None)
    href = None
    if opf:
        opf_text = zf.read(opf).decode("utf-8", errors="replace")
        m = re.search(r'name=["\']cover["\'][^>]*content=["\']([^"\']+)["\']', opf_text, re.I)
        if not m:
            m = re.search(r'content=["\']([^"\']+)["\'][^>]*name=["\']cover["\']', opf_text, re.I)
        idref = m.group(1) if m else None
        if idref:
            m2 = re.search(
                rf'id=["\']{re.escape(idref)}["\'][^>]*href=["\']([^"\']+)["\']',
                opf_text,
                re.I,
            )
            if not m2:
                m2 = re.search(
                    rf'href=["\']([^"\']+)["\'][^>]*id=["\']{re.escape(idref)}["\']',
                    opf_text,
                    re.I,
                )
            if m2:
                href = m2.group(1)
        if not href:
            m3 = re.search(
                r'properties=["\'][^"\']*cover-image[^"\']*["\'][^>]*href=["\']([^"\']+)["\']',
                opf_text,
                re.I,
            )
            if not m3:
                m3 = re.search(
                    r'href=["\']([^"\']+)["\'][^>]*properties=["\'][^"\']*cover-image',
                    opf_text,
                    re.I,
                )
            if m3:
                href = m3.group(1)
        if href:
            base = opf.rsplit("/", 1)[0] + "/" if "/" in opf else ""
            for c in (href, base + href, href.lstrip("./")):
                c = c.replace("\\", "/")
                for n in names:
                    if n == c or n.lower() == c.lower():
                        return zf.read(n)

    best = None
    best_size = 0
    for n in names:
        nl = n.lower()
        if nl.endswith((".jpg", ".jpeg", ".png")):
            size = zf.getinfo(n).file_size
            if size > best_size:
                best_size = size
                best = n
    return zf.read(best) if best else None


def write_1bit_bmp(path: Path, img: Image.Image, width: int, height: int, fit: bool) -> None:
    img = img.convert("L")
    if fit:
        img.thumbnail((width, height), Image.Resampling.LANCZOS)
        canvas = Image.new("L", (width, height), 255)
        canvas.paste(img, ((width - img.width) // 2, (height - img.height) // 2))
        img = canvas
    else:
        img = img.resize((width, height), Image.Resampling.LANCZOS)

    bw = img.point(lambda p: 0 if p < 128 else 255, mode="1")
    row_bytes = ((width + 31) // 32) * 4
    pixel_data = bytearray()
    px = bw.load()
    for y in range(height):
        row = bytearray(row_bytes)
        for x in range(width):
            if px[x, y] != 0:
                row[x // 8] |= 0x80 >> (x % 8)
        pixel_data.extend(row)

    file_size = 14 + 40 + 8 + len(pixel_data)
    out = bytearray()
    out += b"BM"
    out += struct.pack("<IHHI", file_size, 0, 0, 14 + 40 + 8)
    out += struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 1, 0, len(pixel_data), 0, 0, 2, 0)
    out += struct.pack("<BBBB", 0, 0, 0, 0)  # black
    out += struct.pack("<BBBB", 255, 255, 255, 0)  # white
    out += pixel_data
    path.write_bytes(out)


def main() -> None:
    if not ROOT.exists():
        raise SystemExit(f"SD path missing: {ROOT}")

    for logical, physical in BOOKS:
        if not physical.exists():
            print(f"missing epub: {physical}")
            continue
        cache = ROOT / f"epub_{fnv1a64(logical)}"
        cache.mkdir(parents=True, exist_ok=True)
        print(f"Processing {physical.name} -> {cache.name}")
        with zipfile.ZipFile(physical, "r") as zf:
            cover = find_cover_bytes(zf)
        if not cover:
            print("  NO COVER FOUND")
            continue

        src = cache / ("cover_src.png" if cover[:8].startswith(b"\x89PNG") else "cover_src.jpg")
        src.write_bytes(cover)
        im = Image.open(io.BytesIO(cover))
        print(f"  source {src.name} {im.size} {im.mode}")
        for w, h, fit in SIZES:
            name = f"thumb_{w}x{h}_fit.bmp" if fit else f"thumb_{w}x{h}.bmp"
            write_1bit_bmp(cache / name, im, w, h, fit)
            print(f"  wrote {name}")

    # Ensure recent.json cover templates point at path-derived hashes
    recent_path = ROOT / "recent.json"
    if recent_path.exists():
        import json

        data = json.loads(recent_path.read_text(encoding="utf-8"))
        changed = False
        for book in data.get("books", []):
            path = book.get("path", "")
            if not path.lower().endswith(".epub"):
                continue
            want = f"/.crosspoint/epub_{fnv1a64(path)}/thumb_[WIDTH]x[HEIGHT].bmp"
            if book.get("coverBmpPath") != want:
                print(f"  fix coverBmpPath for {path}")
                book["coverBmpPath"] = want
                changed = True
        if changed:
            recent_path.write_text(json.dumps(data, separators=(",", ":")), encoding="utf-8")
            print("updated recent.json")
        else:
            print("recent.json cover paths already ok")

    print("done")


if __name__ == "__main__":
    main()
