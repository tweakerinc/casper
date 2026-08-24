# CrossPoint book store (ownership pillars v1)

## Why

Classic CrossPoint cache keys are **path hashes**:

```
/.crosspoint/epub_<std::hash(fullPath)>/
```

Rename or move a book → new folder → lost progress/stats (or wrong book if collisions).

## Pillars (v1)

| Pillar | Implementation |
|--------|----------------|
| **1. Stable book id** | FNV-1a 64 of `title \| author \| filename-stem` → 16 hex chars |
| **2. Ledger** | `/.casper/ledger.tsv` maps `id ↔ path ↔ title` |
| **3. /.casper file types** | Per-book folder layout below |
| **4. Progress + stats** | Own files under book folder (not path-hash) |

**Reader state** (resume, stats, Rivulet IR) is CrossPoint-owned under `book_<stableId>/`.

**Package parse cache** (`book.bin`, thumbs) still uses path-hash folders
`/.casper/epub_<std::hash(path)>/` (same *scheme* as classic CrossPoint, root is `/.casper`).
Home/stats load probes **stable id first**, then path-hash, then legacy `/.crosspoint`.

## Layout

```
/.casper/
  ledger.tsv
  book_<16hex>/
    meta.txt           # id, path, title, author (debug)
    progress.bin       # 6-byte resume (same layout as classic)
    stats_v6.bin       # BookReadingStats (existing binary format)
    rivulet/
      sN.rvir          # chapter IR
      sN.html          # inflated chapter HTML cache
```

## Identity

```
stableId = hex16( FNV1a64( title, author, fileStem(path) ) )
```

- Title empty → use file stem for the title field.
- Path is **not** in the hash (moves keep the same id).
- Title/author rename can change id (acceptable v1; ledger still updates path).

## Migration

- On first Rivulet open: if CrossPoint stats empty, **import once** via `BookReadingStats::loadForBook(path)` (classic/CrossInk) and save into `book_<id>/`.
- Full CrossInk → CrossPoint bulk convert: deferred external tool (as requested).

## Code

- `src/util/CrossPointBookStore.h/.cpp`
- `RivuletReaderActivity` uses `CrossPointBook::openBook` on enter.

## App root (settings & state)

Canonical device config lives under `/.casper/` (dual-read from `/.crosspoint/` once, then resave):

| File / dir | Purpose |
|------------|---------|
| `settings.json` | All user settings |
| `state.json` | Open book / UI state |
| `recent.json` | Recent books |
| `wifi.json` | Wi‑Fi credentials |
| `opds.json` | OPDS servers |
| `button_map.txt` | Button remap sidecar |
| `koreader.json` | KOReader sync credentials |
| `global_stats.bin` | Lifetime reading stats |
| `synced_stats/` | Multi-device stats imports |
| `bookmarks/` | Per-book bookmark JSON |
| `clippings/` | Per-book clipping stores |
| `sleep_frame.bin` | Quick-resume framebuffer |
| `ota-firmware.bin` | OTA download cache |
| `dict.tmp` | Dictionary extract scratch |
| `book_<id>/` | Stable book progress/stats/IR |

Package EPUB index (`book.bin`, path-hash `epub_*`) under `/.casper` (legacy read from `/.crosspoint`).

## Later

- Move bookmarks/clippings under `book_<id>/`
- Content hash (zip central dir / OPF identifier) for stronger stability
- Retire path-hash for package cache when safe
