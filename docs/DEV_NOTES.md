# Casper — durable development notes

Short repo-specific gotchas worth reusing. Not user-facing docs.

## Dual trees

| Path | Role |
|------|------|
| `C:\Users\m\CrossInk` | Daily worktree (folder name is historical) |
| `E:\casper` | Casper reference for rebasing onto new CrossPoint releases |

**Policy:** product code and docs changes land in both trees. Prefer editing the worktree, then sync to `E:\casper` (or the reverse). See `CASPER.md` / `docs/CASPER_MERGE.md` on the Casper tree.

## Simulator

- Simulator patches belong in the adjacent simulator repo when used.
- Local env name is usually `simulator`.
- Known limits: no full image pipeline; `esp_deep_sleep_start()` may be a no-op; multi-open files may work in sim but not on hardware.

## Hardware / storage

- SdFat on hardware allows only one open reader per file path at a time.
- ESP32-C3: ~380 KB usable RAM, no PSRAM. Stability over features.

## Rendering / reader

- Images in `Page.cpp` render in `GfxRenderer::BW`; grayscale passes are text AA only.
- Kindle EPUBs: skip `<img>` with `data-AmznRemoved-M8` to avoid duplicate images.
- After cache-format or layout pipeline changes, clear `.crosspoint/epub_<hash>/` when testing.

## X3 battery (BQ27220)

- SoC from fuel gauge over I2C (shared with RTC + IMU), not ADC.
- `HalPowerManager` reconciles high SoC vs voltage; look for `BAT` serial logs.

## Dictionary

- Call `DictionaryLookup::beginSession()` / `endSession()` around dictionary UI only.
- Packs live on SD under `/.crosspoint/dict/`.
- Opening dictionary from long-press Menu must ignore residual Confirm hold so multi-word mode does not arm immediately.

## Misc

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()` — duplicate header changes in Lyra if needed.
- Book rename/move: use `BookMoveUtils` so caches/stats follow the file.
