#!/usr/bin/env python3
"""One-shot host migrate: leftover <root>/.casper -> <root>/.crosspoint

Firmware reads and writes /.crosspoint only. This copies files that were
moved into /.casper during the short-lived Casper branding pass, without
overwriting anything already in /.crosspoint.

Usage:
  python scripts/migrate_casper_to_crosspoint.py H:
  python scripts/migrate_casper_to_crosspoint.py /media/sd
  python scripts/migrate_casper_to_crosspoint.py H: --dry-run
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path


MARKER_NAME = "casper_migrate_v1.done"


def iter_files(root: Path):
    for p in root.rglob("*"):
        if p.is_file():
            yield p


def migrate(src_root: Path, dst_root: Path, dry_run: bool) -> tuple[int, int, int]:
    copied = 0
    skipped = 0
    errors = 0

    if not src_root.is_dir():
        print(f"No source dir: {src_root}")
        return 0, 0, 1

    dst_root.mkdir(parents=True, exist_ok=True)

    for src in iter_files(src_root):
        rel = src.relative_to(src_root)
        if rel.name in (MARKER_NAME, "dict.tmp", "crosspoint_migrate_v1.done"):
            skipped += 1
            continue
        dst = dst_root / rel
        if dst.exists():
            skipped += 1
            continue
        try:
            if dry_run:
                print(f"COPY {rel}")
            else:
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
            copied += 1
            if copied % 50 == 0:
                print(f"  … {copied} files copied", flush=True)
        except OSError as e:
            errors += 1
            print(f"ERR  {rel}: {e}", file=sys.stderr)

    return copied, skipped, errors


def main() -> int:
    ap = argparse.ArgumentParser(description="Migrate leftover .casper data into .crosspoint on an SD root")
    ap.add_argument("sd_root", help="SD root (e.g. H: or /media/foo)")
    ap.add_argument("--dry-run", action="store_true", help="List actions only")
    args = ap.parse_args()

    root = Path(args.sd_root)
    root = root.resolve() if root.exists() else Path(str(root).rstrip("\\/") + "\\")
    if not root.exists():
        candidate = Path(args.sd_root.rstrip("\\/") + "\\")
        if candidate.exists():
            root = candidate
        else:
            print(f"SD root not found: {args.sd_root}", file=sys.stderr)
            return 2

    src = root / ".casper"
    dst = root / ".crosspoint"
    marker = dst / MARKER_NAME

    print(f"SD root:     {root}")
    print(f"Source:      {src}  exists={src.is_dir()}")
    print(f"Destination: {dst}  exists={dst.is_dir()}")
    print(f"Marker:      {marker}  exists={marker.exists()}")
    print(f"Mode:        {'DRY-RUN' if args.dry_run else 'WRITE'}")
    print()

    def maybe_rename(src_name: str, dst_name: str) -> None:
        s = root / src_name
        d = root / dst_name
        if not s.is_dir():
            return
        if d.exists():
            print(f"Keep {dst_name} (already present); leftover {src_name} left in place")
            return
        if args.dry_run:
            print(f"RENAME {src_name} -> {dst_name}")
            return
        s.rename(d)
        print(f"Renamed {src_name} -> {dst_name}")

    maybe_rename(".casper-logs", ".crosspoint-logs")
    maybe_rename(".casper-stats-backup", ".crosspoint-stats-backup")

    if not src.is_dir():
        print("No leftover .casper folder — nothing to migrate.")
        return 0

    t0 = time.time()
    copied, skipped, errors = migrate(src, dst, args.dry_run)
    elapsed = time.time() - t0

    print()
    print(f"Done in {elapsed:.1f}s  copied={copied}  skipped_existing={skipped}  errors={errors}")

    if not args.dry_run:
        dst.mkdir(parents=True, exist_ok=True)
        marker.write_text(
            f"v1 host {int(time.time())} copied={copied} skipped={skipped} errors={errors}\n", encoding="utf-8"
        )
        print(f"Wrote marker {marker}")
        for name in ("wifi.json", "settings.json", "recent.json", "global_stats.bin"):
            p = dst / name
            print(f"  check {name}: {'OK' if p.exists() else 'MISSING'}")

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
