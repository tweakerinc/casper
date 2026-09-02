#!/usr/bin/env python3
"""
One-shot: copy progress/stats from book_<fnv> → epub_<ESP32 std::hash>
for Dungeon Crawler Carl (or any book_* with meta path).

ESP32-C3 std::hash<std::string> = libstdc++ 32-bit _Hash_bytes seed 0xC70F6907.

Usage:
  python book_to_epub_dcc.py "H:\\" [--dry-run]
"""

from __future__ import annotations

import argparse
import shutil
import struct
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def hash_bytes_32(data: bytes, seed: int = 0xC70F6907) -> int:
    """libstdc++ _Hash_bytes unaligned 32-bit (ESP32 size_t)."""
    m = 0x5BD1E995
    r = 24
    length = len(data)
    h = (seed ^ length) & 0xFFFFFFFF
    i = 0
    while length >= 4:
        k = int.from_bytes(data[i : i + 4], "little")
        k = (k * m) & 0xFFFFFFFF
        k ^= k >> r
        k = (k * m) & 0xFFFFFFFF
        h = (h * m) & 0xFFFFFFFF
        h ^= k
        i += 4
        length -= 4
    if length == 3:
        h ^= data[i + 2] << 16
    if length >= 2:
        h ^= data[i + 1] << 8
    if length >= 1:
        h ^= data[i]
        h = (h * m) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * m) & 0xFFFFFFFF
    h ^= h >> 15
    return h & 0xFFFFFFFF


def epub_dir_name(device_path: str) -> str:
    # Device paths are absolute from SD root with leading '/'.
    p = device_path.replace("\\", "/")
    if not p.startswith("/"):
        p = "/" + p
    return "epub_" + str(hash_bytes_32(p.encode("utf-8")))


def parse_meta(meta_path: Path) -> Dict[str, str]:
    out: Dict[str, str] = {}
    if not meta_path.is_file():
        return out
    for line in meta_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def stats_richness(stats_path: Path) -> Tuple[int, int, int]:
    """(seconds, sessions, pages) for compare; zeros if unreadable."""
    try:
        b = stats_path.read_bytes()
        if len(b) < 11:
            return (0, 0, 0)
        sessions = struct.unpack_from("<H", b, 1)[0]
        seconds = struct.unpack_from("<I", b, 3)[0]
        pages = struct.unpack_from("<I", b, 7)[0]
        return (seconds, sessions, pages)
    except OSError:
        return (0, 0, 0)


def prefer_copy(src: Path, dst: Path, dry: bool) -> str:
    """Copy src→dst if dst missing or src is richer (for stats) / always for progress if src exists."""
    if not src.is_file():
        return "skip-no-src"
    if src.name.startswith("stats") and src.suffix == ".bin":
        if dst.is_file():
            s, d = stats_richness(src), stats_richness(dst)
            if s <= d:
                return f"keep-dst {d}"
        action = "overwrite" if dst.is_file() else "create"
    else:
        action = "overwrite" if dst.is_file() else "create"
    if not dry:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    return action


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sd_root", type=Path, help="SD root (contains .crosspoint)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument(
        "--filter",
        default="Dungeon Crawler Carl",
        help="Only convert book_* whose meta path contains this (empty = all with meta path)",
    )
    args = ap.parse_args()
    root = args.sd_root
    cp = root / ".crosspoint"
    if not cp.is_dir():
        print(f"ERROR: no {cp}")
        return 1

    books = sorted(cp.glob("book_*"))
    converted = 0
    for book_dir in books:
        meta = parse_meta(book_dir / "meta.txt")
        path = meta.get("path") or ""
        title = meta.get("title") or book_dir.name
        if not path:
            continue
        if args.filter and args.filter not in path and args.filter not in title:
            continue

        epub_name = epub_dir_name(path)
        epub_dir = cp / epub_name
        print(f"\n{book_dir.name}")
        print(f"  path  {path}")
        print(f"  title {title}")
        print(f"  → {epub_name}/")

        # Copy progress + all stats*.bin (not the scanned marker)
        for name in ("progress.bin", "progress.bin.bak"):
            src = book_dir / name
            if src.is_file():
                r = prefer_copy(src, epub_dir / name, args.dry_run)
                print(f"  {name}: {r}")

        for src in sorted(book_dir.glob("stats*.bin")):
            r = prefer_copy(src, epub_dir / src.name, args.dry_run)
            rich = stats_richness(src)
            print(f"  {src.name}: {r}  (secs={rich[0]} sessions={rich[1]} pages={rich[2]})")

        # Touch marker so we know conversion ran (optional, harmless)
        if not args.dry_run:
            epub_dir.mkdir(parents=True, exist_ok=True)
            mark = epub_dir / "stats_from_book_dir.txt"
            mark.write_text(f"from={book_dir.name}\npath={path}\n", encoding="utf-8")

        converted += 1

    print(f"\nDone. Converted {converted} book(s). dry_run={args.dry_run}")
    print("global_stats.bin left as-is (already under .crosspoint).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
