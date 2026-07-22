# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## Simulator

- Simulator patches belong in the adjacent `crosspoint-simulator` repo.
- The valid local simulator env in this repo is `simulator`, and `pio run -e simulator` currently builds cleanly.
- The simulator `PNGdec` stub in `crosspoint-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## Heap Baselines (X4 hardware, SD card font)

- A normal resume-into-partial reading session runs at ~85-90KB free / ~49KB maxAlloc by
  the first watermark crossing (Epub metadata + x-locations + resident glyph caches).
  Do not read mid-range heap numbers as session degradation without checking the scenario.
- SD-font section builds cost ~38-50KB at cold start; the 4-style advance-table prewarm
  (~30KB incl. 16KB contiguous scratch) dominates and is skipped below 80KB free.

## X3 Battery (BQ27220)

- X3 SoC comes from the BQ27220 fuel gauge over I2C (shared with RTC + IMU), not ADC.
- Unlearned / wrong design-capacity packs often stick near 100% until a full charge→empty→charge cycle.
- `HalPowerManager` cross-checks SoC against gauge voltage (`percentageFromMillivolts`) when SoC is high but mV is not full; uses a longer Wire timeout (50 ms) and smooths display %.
- Serial: look for `BAT` logs `gauge SoC=… V=… I=… Vmap=… -> show=…`.

## Misc Repo Gotchas

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.
- Dictionary packs: call `DictionaryLookup::beginSession()` / `endSession()` around dictionary UI only. Lookups reuse open files; never scan packs on Home.
- Book rename/move: use `BookMoveUtils::migrateRenamedBook` (or `migrateMovedEpubState`) so `epub_<hash>` / stats / thumbs follow the file. Web rename used to `clearBookCache` first and wiped progress.
- Serial freeze debug: `Activity` logs enter/exit free heap; panic dump includes `Last activity:` from `getLastActivityName()`.
- Dashboard/Minimal covers: Home generates fixed-size adaptive thumbs (Dashboard `296x444_fit`, Minimal metrics). Draw must accept those files even when the live cover rect is smaller (title + lifetime reserve), then scale with `fittedBitmapRect`. After `loadRecentCovers`, always clear the home cover buffer snapshot or a “missing cover” frame sticks forever.
