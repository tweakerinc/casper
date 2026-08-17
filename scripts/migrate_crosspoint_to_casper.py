#!/usr/bin/env python3
"""One-shot host migrate: <root>/.crosspoint -> <root>/.casper

Same policy as on-device CasperOneTimeMigrate:
  - Never overwrite an existing Casper file
  - Fill missing files inside shared dirs (e.g. thin epub_* under .casper)
  - Write /.casper/crosspoint_migrate_v1.done when finished

Usage:
  python scripts/migrate_crosspoint_to_casper.py H:
  python scripts/migrate_crosspoint_to_casper.py /media/sd
  python scripts/migrate_crosspoint_to_casper.py H: --dry-run
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path


MARKER_NAME = "crosspoint_migrate_v1.done"


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
        # Skip transient junk
        if rel.name == "dict.tmp":
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
    ap = argparse.ArgumentParser(description="Migrate .crosspoint → .casper on an SD root")
    ap.add_argument("sd_root", help="SD root (e.g. H: or /media/foo)")
    ap.add_argument("--dry-run", action="store_true", help="List actions only")
    ap.add_argument("--force-marker", action="store_true", help="Write marker even if source missing")
    args = ap.parse_args()

    root = Path(args.sd_root)
    # Windows "H:" is a drive; Path("H:") / ".casper" can be odd — normalize.
    root = root.resolve() if root.exists() else Path(str(root).rstrip("\\/") + "\\")
    if not root.exists():
        # Drive letter alone
        candidate = Path(args.sd_root.rstrip("\\/") + "\\")
        if candidate.exists():
            root = candidate
        else:
            print(f"SD root not found: {args.sd_root}", file=sys.stderr)
            return 2

    src = root / ".crosspoint"
    dst = root / ".casper"
    marker = dst / MARKER_NAME

    print(f"SD root:     {root}")
    print(f"Source:      {src}  exists={src.is_dir()}")
    print(f"Destination: {dst}  exists={dst.is_dir()}")
    print(f"Marker:      {marker}  exists={marker.exists()}")
    print(f"Mode:        {'DRY-RUN' if args.dry_run else 'WRITE'}")
    print()

    if marker.exists() and not args.dry_run:
        print("Marker already present — one-shot already considered done.")
        print("Delete the marker and re-run if you want to fill remaining gaps.")
        # Still allow fill-in of missing files even with marker? User said things
        # weren't migrated — re-run fill without requiring delete if we always
        # merge missing. Proceed with merge; refresh marker after.
        print("Continuing with fill-missing merge anyway…")

    if not src.is_dir():
        print("No .crosspoint folder — nothing to migrate.")
        if not args.dry_run and (args.force_marker or True):
            dst.mkdir(parents=True, exist_ok=True)
            marker.write_text(f"v1 host no-source {int(time.time())}\n", encoding="utf-8")
            print(f"Wrote marker {marker}")
        return 0

    t0 = time.time()
    copied, skipped, errors = migrate(src, dst, args.dry_run)
    elapsed = time.time() - t0

    print()
    print(f"Done in {elapsed:.1f}s  copied={copied}  skipped_existing={skipped}  errors={errors}")

    if not args.dry_run:
        dst.mkdir(parents=True, exist_ok=True)
        marker.write_text(f"v1 host {int(time.time())} copied={copied} skipped={skipped} errors={errors}\n", encoding="utf-8")
        print(f"Wrote marker {marker}")

        # Spot-check important files
        for name in ("wifi.json", "settings.json", "recent.json", "global_stats.bin"):
            p = dst / name
            print(f"  check {name}: {'OK' if p.exists() else 'MISSING'}")
        epub_c = len(list(dst.glob("epub_*")))
        epub_x = len(list(src.glob("epub_*")))
        print(f"  epub_* dirs: casper={epub_c} crosspoint={epub_x}")

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
