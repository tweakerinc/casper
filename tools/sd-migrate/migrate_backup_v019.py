#!/usr/bin/env python3
"""
Host migration: SD backup → Casper v0.1.9 layout under /.crosspoint

Contract matches lib/FsHelpers/BookPathId.{h,cpp}:
  normalizePath + FNV-1a 64 → book_<hex16>/

What this does:
  1. Prefer newer config files from .casper over .crosspoint when merging.
  2. Map path → legacy epub_* via recent.json coverBmpPath (+ ledger/meta).
  3. Materialize book_<id>/{package,rivulet,progress,stats,meta.txt}.
  4. Move package contents from epub_* into package/ (prefer richer source).
  5. Rewrite recent.json cover paths to /.crosspoint/book_<id>/package/...
  6. Write crosspoint_migrate_v2.done marker.
  7. Delete .casper entirely.

Usage:
  python migrate_backup_v019.py "E:\\path\\to\\SD root"
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple

BOOK_EXT = {".epub", ".xtc", ".xtch", ".txt", ".md"}
SKIP_TOP = {
    ".casper",
    "casper",
    ".crosspoint",
    "crosspoint",
    ".metadata",
    "metadata",
    ".system",
    "system volume information",
    ".dictionaries",
    ".fonts",
    ".sleep",
    ".casper-logs",
    ".casper-stats-backup",
    ".crosspoint-stats-backup",
}

CFG_FILES = [
    "settings.json",
    "wifi.json",
    "recent.json",
    "state.json",
    "opds.json",
    "koreader.json",
    "button_map.txt",
    "global_stats.bin",
    "global_stats.bin.bak",
    "sleep_frame.bin",
    "koreader_profiles.json",
    "achievements.json",
    "reading_stats.json",
    "reading_stats.json.bak",
    "crossink-settings.json",
    "ledger.tsv",
]

LIB_DIRS = ["clippings", "bookmarks", "synced_stats", "bookdata"]

# Files that live under book_<id>/package/
PACKAGE_NAMES = {
    "book.bin",
    "cover.bmp",
    "thumb.bmp",
    "css_rules.cache",
    "description.html",
}
PACKAGE_PREFIXES = ("thumb_", "cover_", "img_")
PACKAGE_DIRS = ("html", "sections", "images")

# Files that live at book_<id>/ root
BOOK_ROOT_FILES = {
    "progress.bin",
    "progress.bin.bak",
    "stats_v6.bin",
    "stats_v5.bin",
    "stats_v4.bin",
    "stats_v3.bin",
    "stats_v2.bin",
    "stats_v1.bin",
    "stats.bin",
    "statistics.bin",
    "stats_legacy_scanned",
    "meta.txt",
}

EPUB_RE = re.compile(r"epub_\d+", re.I)


def normalize_path(path: str) -> str:
    if not path:
        return ""
    s = str(path).replace("\\", "/")
    if len(s) >= 2 and s[0].isalpha() and s[1] == ":":
        s = s[2:]
    while s.startswith("//"):
        s = s[1:]
    while s.startswith("/"):
        s = s[1:]
    while s.endswith("/") and len(s) > 1:
        s = s[:-1]
    parts: List[str] = []
    for p in s.split("/"):
        if not p or p == ".":
            continue
        if p == "..":
            if parts:
                parts.pop()
            continue
        parts.append(p)
    if not parts:
        return "/"
    return "/" + "/".join(parts)


def fnv1a64(s: str) -> int:
    h = 14695981039346656037
    prime = 1099511628211
    mask = 0xFFFFFFFFFFFFFFFF
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * prime) & mask
    return h


def id_hex(path: str) -> str:
    return f"{fnv1a64(normalize_path(path)):016x}"


def book_dir_name(path: str) -> str:
    return "book_" + id_hex(path)


def extract_epub_folder(p: str) -> Optional[str]:
    if not p:
        return None
    s = str(p).replace("\\", "/")
    m = EPUB_RE.search(s)
    if m:
        return m.group(0)
    m = re.search(r"/epub/(\d+)/", s, re.I)
    if m:
        return "epub_" + m.group(1)
    return None


def dir_size(p: Path) -> int:
    total = 0
    if not p.exists():
        return 0
    for f in p.rglob("*"):
        if f.is_file():
            try:
                total += f.stat().st_size
            except OSError:
                pass
    return total


def file_mtime(p: Path) -> float:
    try:
        return p.stat().st_mtime
    except OSError:
        return 0.0


def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def copy_file(src: Path, dst: Path, *, overwrite: bool = False) -> str:
    """Copy src→dst. Returns 'copied'|'skipped'|'overwritten'."""
    if not src.is_file():
        return "missing"
    ensure_dir(dst.parent)
    if dst.exists():
        if not overwrite:
            return "skipped"
        # Prefer larger/newer when overwriting is allowed and dest is smaller
        try:
            if dst.stat().st_size >= src.stat().st_size and file_mtime(dst) >= file_mtime(src):
                return "skipped"
        except OSError:
            pass
        shutil.copy2(src, dst)
        return "overwritten"
    shutil.copy2(src, dst)
    return "copied"


def merge_tree(src: Path, dst: Path, *, prefer_src: bool = False) -> Tuple[int, int]:
    """Copy files from src into dst; never delete. Returns (copied, skipped)."""
    copied = skipped = 0
    if not src.exists():
        return 0, 0
    for f in src.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(src)
        dest = dst / rel
        if dest.exists() and not prefer_src:
            skipped += 1
            continue
        if dest.exists() and prefer_src:
            # keep dest if larger and as new
            try:
                if dest.stat().st_size > f.stat().st_size:
                    skipped += 1
                    continue
            except OSError:
                pass
        ensure_dir(dest.parent)
        shutil.copy2(f, dest)
        copied += 1
    return copied, skipped


def find_books(sd_root: Path) -> List[str]:
    out: List[str] = []
    for child in sd_root.iterdir():
        name = child.name
        if name.lower() in SKIP_TOP:
            continue
        if child.is_file() and child.suffix.lower() in BOOK_EXT:
            out.append(normalize_path("/" + name))
        elif child.is_dir():
            for f in child.rglob("*"):
                if not f.is_file():
                    continue
                if f.suffix.lower() not in BOOK_EXT:
                    continue
                # skip hidden subtrees
                parts_lower = {p.lower() for p in f.relative_to(sd_root).parts}
                if parts_lower & SKIP_TOP:
                    continue
                rel = f.relative_to(sd_root).as_posix()
                out.append(normalize_path("/" + rel))
    out = sorted(set(out))
    return out


def load_json(path: Path) -> Optional[dict]:
    try:
        with path.open("r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def collect_path_epub_map(sd_root: Path) -> Dict[str, str]:
    """path → epub_XXXX from recent.json cover fields."""
    cover_map: Dict[str, str] = {}
    for side in (".casper", ".crosspoint"):
        recent = sd_root / side / "recent.json"
        doc = load_json(recent)
        if not doc:
            continue
        arr = doc.get("books") or doc.get("recent") or (doc if isinstance(doc, list) else [])
        if not isinstance(arr, list):
            continue
        for b in arr:
            if not isinstance(b, dict):
                continue
            p = normalize_path(b.get("path") or "")
            ep = extract_epub_folder(b.get("coverBmpPath") or b.get("cover") or "")
            if p and ep:
                cover_map[p] = ep
    return cover_map


def collect_paths_from_config(sd_root: Path) -> List[str]:
    paths: List[str] = []
    for side in (".casper", ".crosspoint"):
        base = sd_root / side
        recent = load_json(base / "recent.json")
        if recent:
            arr = recent.get("books") or recent.get("recent") or []
            if isinstance(arr, list):
                for b in arr:
                    if isinstance(b, dict) and b.get("path"):
                        paths.append(normalize_path(b["path"]))
        state = load_json(base / "state.json")
        if state:
            for key in ("openEpubPath", "openPath", "currentBook", "lastBook"):
                v = state.get(key)
                if isinstance(v, str) and v:
                    paths.append(normalize_path(v))
        ledger = base / "ledger.tsv"
        if ledger.is_file():
            try:
                text = ledger.read_text(encoding="utf-8", errors="replace")
            except OSError:
                text = ""
            for line in text.splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) >= 2 and parts[1]:
                    paths.append(normalize_path(parts[1]))
        # meta.txt under book_*
        for bdir in base.glob("book_*"):
            meta = bdir / "meta.txt"
            if not meta.is_file():
                continue
            try:
                for line in meta.read_text(encoding="utf-8", errors="replace").splitlines():
                    if line.startswith("path="):
                        paths.append(normalize_path(line[5:]))
            except OSError:
                pass
    # unique preserve order
    seen: Set[str] = set()
    out: List[str] = []
    for p in paths:
        if p and p not in seen:
            seen.add(p)
            out.append(p)
    return out


def title_from_path(path: str) -> str:
    name = Path(path).name
    for ext in BOOK_EXT:
        if name.lower().endswith(ext):
            name = name[: -len(ext)]
            break
    return name


def classify_and_place(src_file: Path, book_root: Path, package: Path) -> Path:
    """Decide destination path for a file found under a legacy epub_* dir."""
    name = src_file.name
    # relative parts under epub
    # If under html/ or sections/, keep structure under package
    try:
        # find epub parent — walk up looking for epub_
        cur = src_file.parent
        epub_root = None
        for _ in range(8):
            if cur.name.startswith("epub_"):
                epub_root = cur
                break
            if cur.parent == cur:
                break
            cur = cur.parent
        rel_parts = src_file.relative_to(epub_root).parts if epub_root else (name,)
    except ValueError:
        rel_parts = (name,)

    if name in BOOK_ROOT_FILES or name.startswith("stats"):
        # Normalize older stats filenames to stats_v6.bin (firmware import expects v6).
        if name in ("stats.bin", "stats_v5.bin", "statistics.bin"):
            dst_name = "stats_v6.bin"
        else:
            dst_name = name
        return book_root / dst_name

    if name == "meta.txt":
        return book_root / "meta.txt"

    # package content
    if len(rel_parts) > 1:
        return package.joinpath(*rel_parts)
    if name in PACKAGE_NAMES or name.startswith(PACKAGE_PREFIXES) or name.endswith((".pxc7", ".pxc10", ".jpg", ".png", ".gif", ".bmp")):
        return package / name
    # default: package
    return package / name


def import_epub_dir(epub_dir: Path, book_root: Path) -> Tuple[int, int]:
    package = book_root / "package"
    ensure_dir(package)
    ensure_dir(book_root / "rivulet")
    copied = skipped = 0
    if not epub_dir.is_dir():
        return 0, 0
    for f in epub_dir.rglob("*"):
        if not f.is_file():
            continue
        # skip nested junk
        if f.name in ("dict.tmp",):
            continue
        dst = classify_and_place(f, book_root, package)
        # progress/stats: prefer non-empty larger
        overwrite = False
        if dst.name.startswith("stats") or dst.name.startswith("progress"):
            overwrite = True  # allow upgrade from empty/small
        res = copy_file(f, dst, overwrite=overwrite and (not dst.exists() or f.stat().st_size > dst.stat().st_size))
        if res in ("copied", "overwritten"):
            copied += 1
        else:
            skipped += 1
    return copied, skipped


def import_existing_book_dir(src_book: Path, dst_book: Path) -> Tuple[int, int]:
    """Merge an existing book_* tree (from .casper) into .crosspoint/book_*."""
    ensure_dir(dst_book)
    ensure_dir(dst_book / "package")
    ensure_dir(dst_book / "rivulet")
    return merge_tree(src_book, dst_book, prefer_src=True)


def merge_config(sd_root: Path, dst: Path) -> None:
    casper = sd_root / ".casper"
    cross = sd_root / ".crosspoint"
    ensure_dir(dst)

    # Config files: prefer newer mtime; if equal prefer .casper (WIP was newer product path)
    for name in CFG_FILES:
        candidates = []
        for base in (casper, cross):
            p = base / name
            if p.is_file():
                candidates.append(p)
        if not candidates:
            continue
        # pick newest
        best = max(candidates, key=lambda p: (file_mtime(p), 1 if ".casper" in str(p) else 0, p.stat().st_size))
        dest = dst / name
        if dest.exists() and dest.resolve() == best.resolve():
            continue
        if dest.exists() and file_mtime(dest) >= file_mtime(best) and dest.stat().st_size >= best.stat().st_size:
            continue
        shutil.copy2(best, dest)
        print(f"  config: {name} ← {best.parent.name} ({best.stat().st_size}B)")

    # Library dirs: merge both, prefer larger files
    for lib in LIB_DIRS:
        for base in (casper, cross):
            src = base / lib
            if src.is_dir():
                c, s = merge_tree(src, dst / lib, prefer_src=False)
                if c:
                    print(f"  lib {lib} from {base.name}: +{c} files")


def rewrite_recent_covers(recent_path: Path, path_to_id: Dict[str, str]) -> int:
    doc = load_json(recent_path)
    if not doc:
        return 0
    arr = doc.get("books") or doc.get("recent")
    if not isinstance(arr, list):
        return 0
    changed = 0
    for b in arr:
        if not isinstance(b, dict):
            continue
        p = normalize_path(b.get("path") or "")
        bid = path_to_id.get(p) or (id_hex(p) if p else "")
        cover = b.get("coverBmpPath") or b.get("cover") or ""
        if not bid or not cover:
            continue
        # rewrite /.crosspoint/epub_X/foo → /.crosspoint/book_<id>/package/foo
        # also /.casper/...
        new_cover = cover
        m = re.search(r"(?:/\.crosspoint|/\.casper)/(epub_\d+|book_[0-9a-f]+)/(.*)$", cover.replace("\\", "/"), re.I)
        if m:
            leaf = m.group(2)
            # if leaf already under package/, keep; else prefix package/
            if leaf.startswith("package/"):
                new_cover = f"/.crosspoint/book_{bid}/{leaf}"
            else:
                new_cover = f"/.crosspoint/book_{bid}/package/{leaf}"
        elif extract_epub_folder(cover):
            # fallback: basename only
            leaf = Path(cover.replace("\\", "/")).name
            new_cover = f"/.crosspoint/book_{bid}/package/{leaf}"
        if new_cover != cover:
            if "coverBmpPath" in b:
                b["coverBmpPath"] = new_cover
            elif "cover" in b:
                b["cover"] = new_cover
            else:
                b["coverBmpPath"] = new_cover
            changed += 1
    if changed:
        with recent_path.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, ensure_ascii=False, separators=(",", ":"))
    return changed


def write_meta(book_root: Path, bid: str, path: str, title: str = "", author: str = "") -> None:
    meta = book_root / "meta.txt"
    # don't overwrite richer meta
    if meta.is_file():
        try:
            existing = meta.read_text(encoding="utf-8", errors="replace")
            if "path=" in existing and path in existing:
                return
        except OSError:
            pass
    lines = [f"id={bid}", f"path={normalize_path(path)}", f"title={title or title_from_path(path)}", f"author={author}"]
    meta.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_ledger(dst: Path, entries: List[Tuple[str, str, str]]) -> None:
    """entries: (id, path, title)"""
    ledger_path = dst / "ledger.tsv"
    existing: Dict[str, Tuple[str, str]] = {}
    if ledger_path.is_file():
        for line in ledger_path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split("\t")
            if parts:
                existing[parts[0]] = (parts[1] if len(parts) > 1 else "", parts[2] if len(parts) > 2 else "")
    for bid, path, title in entries:
        existing[bid] = (normalize_path(path), title)
    lines = [f"{bid}\t{path}\t{title}" for bid, (path, title) in sorted(existing.items())]
    ledger_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def pick_epub_source(sd_root: Path, epub_name: str) -> Optional[Path]:
    """Choose the richer of .casper/epub_X vs .crosspoint/epub_X."""
    candidates = []
    for side in (".casper", ".crosspoint"):
        p = sd_root / side / epub_name
        if p.is_dir():
            candidates.append((dir_size(p), p))
    if not candidates:
        return None
    candidates.sort(key=lambda x: x[0], reverse=True)
    return candidates[0][1]


def main() -> int:
    ap = argparse.ArgumentParser(description="Migrate SD backup to Casper v0.1.9 /.crosspoint layout")
    ap.add_argument("sd_root", type=Path, help="Path to SD backup root (contains books + .casper/.crosspoint)")
    ap.add_argument("--keep-casper", action="store_true", help="Do not delete .casper after migration")
    ap.add_argument("--keep-legacy-epub", action="store_true", help="Do not remove epub_* dirs after rekey")
    args = ap.parse_args()

    sd_root = args.sd_root.resolve()
    if not sd_root.is_dir():
        print(f"ERROR: not a directory: {sd_root}", file=sys.stderr)
        return 1

    casper = sd_root / ".casper"
    cross = sd_root / ".crosspoint"
    if not casper.is_dir() and not cross.is_dir():
        print("ERROR: neither .casper nor .crosspoint found", file=sys.stderr)
        return 1

    dst = cross
    ensure_dir(dst)
    print(f"SD root: {sd_root}")
    print(f"Destination: {dst}")

    print("\n[1/6] Merge config + libraries into .crosspoint")
    merge_config(sd_root, dst)

    print("\n[2/6] Discover books")
    books_on_sd = find_books(sd_root)
    config_paths = collect_paths_from_config(sd_root)
    cover_map = collect_path_epub_map(sd_root)
    all_paths: List[str] = []
    seen: Set[str] = set()
    for p in books_on_sd + config_paths:
        n = normalize_path(p)
        if n and n not in seen:
            seen.add(n)
            all_paths.append(n)
    print(f"  books on SD: {len(books_on_sd)}")
    print(f"  paths from config/ledger: {len(config_paths)}")
    print(f"  unique paths: {len(all_paths)}")
    print(f"  path→epub map: {len(cover_map)}")

    # titles/authors from recent
    meta_info: Dict[str, Tuple[str, str]] = {}
    for side in (".casper", ".crosspoint"):
        doc = load_json(sd_root / side / "recent.json")
        if not doc:
            continue
        for b in doc.get("books") or []:
            if isinstance(b, dict) and b.get("path"):
                p = normalize_path(b["path"])
                meta_info[p] = (b.get("title") or "", b.get("author") or "")

    print("\n[3/6] Materialize book_<id> and rekey packages")
    ledger_entries: List[Tuple[str, str, str]] = []
    path_to_id: Dict[str, str] = {}
    used_epubs: Set[str] = set()
    total_copied = 0

    for path in all_paths:
        bid = id_hex(path)
        bname = "book_" + bid
        book_root = dst / bname
        ensure_dir(book_root / "package")
        ensure_dir(book_root / "rivulet")
        path_to_id[path] = bid
        title, author = meta_info.get(path, ("", ""))
        if not title:
            title = title_from_path(path)
        write_meta(book_root, bid, path, title, author)
        ledger_entries.append((bid, path, title))

        # merge existing book_* from .casper if same id
        for side in (".casper", ".crosspoint"):
            src_b = sd_root / side / bname
            if src_b.is_dir() and src_b.resolve() != book_root.resolve():
                c, s = import_existing_book_dir(src_b, book_root)
                total_copied += c
                if c:
                    print(f"  merged {side}/{bname}: +{c} files")

        epub = cover_map.get(path)
        if epub:
            used_epubs.add(epub)
            src = pick_epub_source(sd_root, epub)
            if src:
                c, s = import_epub_dir(src, book_root)
                total_copied += c
                print(f"  {path}")
                print(f"    → {bname}  ← {src.parent.name}/{epub} (+{c} files)")
            else:
                print(f"  {path} → {bname} (no epub dir for {epub})")
        else:
            print(f"  {path} → {bname} (no legacy epub mapping; package may rebuild on device)")

    # Also import any book_* dirs under .casper that we haven't seen (orphan progress)
    if casper.is_dir():
        for src_b in casper.glob("book_*"):
            if not src_b.is_dir():
                continue
            bid = src_b.name[len("book_") :]
            # if already handled as path, still merge
            dst_b = dst / src_b.name
            if dst_b.resolve() != src_b.resolve():
                c, s = import_existing_book_dir(src_b, dst_b)
                total_copied += c
                if c:
                    print(f"  orphan merge {src_b.name}: +{c}")

    print("\n[4/6] Write ledger + marker; rewrite recent covers")
    write_ledger(dst, ledger_entries)
    recent = dst / "recent.json"
    n = rewrite_recent_covers(recent, path_to_id)
    print(f"  recent cover paths rewritten: {n}")

    marker = dst / "crosspoint_migrate_v2.done"
    marker.write_text(
        f"v2 path-id host-python {datetime.now(timezone.utc).isoformat()} "
        f"books={len(all_paths)} copied≈{total_copied}\n",
        encoding="utf-8",
    )
    # drop v1 marker
    v1 = dst / "crosspoint_migrate_v1.done"
    if v1.exists():
        v1.unlink()
    print(f"  wrote {marker.name}")

    print("\n[5/6] Remove legacy epub_* under .crosspoint (rekeyed into book_*)")
    if not args.keep_legacy_epub:
        removed = 0
        for epub_dir in list(dst.glob("epub_*")):
            if epub_dir.is_dir():
                shutil.rmtree(epub_dir)
                removed += 1
        print(f"  removed {removed} epub_* dirs from .crosspoint")
    else:
        print("  kept epub_* (--keep-legacy-epub)")

    print("\n[6/6] Delete .casper")
    if casper.is_dir() and not args.keep_casper:
        shutil.rmtree(casper)
        print("  deleted .casper")
    elif args.keep_casper:
        print("  kept .casper (--keep-casper)")
    else:
        print("  no .casper to delete")

    # Summary
    book_dirs = sorted(dst.glob("book_*"))
    print("\n=== DONE ===")
    print(f"  book_* dirs: {len(book_dirs)}")
    print(f"  remaining epub_* in .crosspoint: {len(list(dst.glob('epub_*')))}")
    print(f"  .casper exists: {(sd_root / '.casper').exists()}")
    print(f"  marker: {marker.exists()}")
    for b in book_dirs[:12]:
        pkg = b / "package"
        nfiles = sum(1 for _ in b.rglob("*") if _.is_file())
        has_prog = (b / "progress.bin").is_file()
        has_stats = (b / "stats_v6.bin").is_file() or (b / "stats.bin").is_file()
        has_book = (pkg / "book.bin").is_file()
        print(f"  {b.name}: files={nfiles} book.bin={has_book} progress={has_prog} stats={has_stats}")
    if len(book_dirs) > 12:
        print(f"  ... +{len(book_dirs) - 12} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
