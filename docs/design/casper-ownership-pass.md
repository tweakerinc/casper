# Casper ownership pass (2026-08)

## Goals
- `/.casper` owns all **new** writes (config, package cache, book state).
- `/.crosspoint` is **read/migrate only** — never `mkdir`.
- Rivulet is the only EPUB reader (classic Section painter parked).
- Cut unused product surface (auto page turn).

## On disk

| What | Path |
|------|------|
| Settings, wifi, recent, state, OPDS, KOReader | `/.casper/*.json` |
| Button map sidecar | `/.casper/button_map.txt` |
| Package cache (`book.bin`, thumbs) | `/.casper/epub_<hash>/` |
| Rivulet IR + progress + per-book stats | `/.casper/book_<id>/` |
| Legacy (read if present, then migrate) | `/.crosspoint/…` |

**One-shot migrate (preferred):** `CasperOneTimeMigrate::runOnceIfNeeded()` runs at boot after SD init, **before** settings load. It moves/copies everything under `/.crosspoint` into `/.casper` (never overwrites existing Casper files), then writes:

```
/.casper/crosspoint_migrate_v1.done
```

Later boots only check that marker — no recursive scan, no per-book forever dual-read. Leftover dual-read paths in stores are a thin safety net if a file was missed.

Open path: prefer `/.casper` `book.bin`; if missing, open existing legacy `/.crosspoint` cache once (no re-index tax). After the one-shot pass, package `epub_*` should already live under `/.casper`.

## Reader
- `ReaderActivity` always opens `RivuletReaderActivity`.
- Classic `EpubReaderActivity` + `ClipSelectionActivity` live in `_parked_classic_reader/` (not compiled).
- Auto page turn removed from reader menu.
- Layout: default body justify; user force-align encoded in `RenderKey.flags`.

## Flash
- Parked classic reader (~200 KB source). Binary drop is modest if LTO already dropped unused symbols; build is cleaner and cannot re-link classic by accident.

## Still open (styling / size)
- In-book **images** not in Rivulet IR yet.
- Full publisher **CSS** not consumed (tag-based styling only).
- Hyphenation / bionic / guide flags are fingerprint-only until layouter implements them.
- Home cover multipass still classic Atkinson (not yet slimmed).
- Further size: OPDS / Calibre / multi-language packs if product agrees.
