# Casper

Personal firmware project based on **CrossPoint Reader** for Xteink X3/X4 (ESP32-C3).

**Product branding is Casper only** — no CrossInk user-facing names or logos in UI, boot, web, or docs.

This repo at `E:\casper` is the **Casper reference tree**: branding, defaults, dashboard, dictionary, KOReader sync, and UI tweaks to keep when rebasing onto new CrossPoint releases.

## Dual-tree sync (always)

| Path | Role |
|------|------|
| `C:\Users\m\CrossInk` | Daily worktree (legacy folder name only) |
| `E:\casper` | Reference for CrossPoint rebases |

After any product change:

```bat
robocopy C:\Users\m\CrossInk E:\casper /E /XD .git .pio /XF tmp_title_chunk.txt /NFL /NDL /NJH /NJS
```

Then commit here on `casper/reference` when the change should be in the merge baseline.

## Why this folder exists

CrossPoint ships new firmware periodically. Merge path:

1. Take the **new CrossPoint base**
2. Re-apply **Casper deltas** from this tree (not a blind full overwrite)
3. Build and flash Casper

See **[docs/CASPER_MERGE.md](docs/CASPER_MERGE.md)**.

## Must-keep Casper features

### Branding
- Product name **Casper** (boot logo, serial, web titles, device name defaults, User-Agent)
- Boot logo under `src/images/`
- Web chrome via `scripts/build_web.py` / `web/`

### Defaults (`src/CrossPointSettings.h`)
| Setting | Default | Intent |
|---------|---------|--------|
| `shortPwrBtn` | `SLEEP` | Short power = sleep |
| `longPressMenuAction` | `LONG_MENU_DICTIONARY` | Long-press menu → dictionary |
| `sideButtonLongPress` | `SIDE_LONG_OFF` | No multi-page on side hold |

### Dashboard
- `src/components/themes/dashboard/`
- Home integration in `HomeActivity*`

### Dictionary
- `lib/Dictionary/`
- `DictionarySelectionActivity` / `DictionaryLookupActivity`
- Opens in **word cursor mode** (Up/Down/Left/Right to find a word; short Select looks up)
- Long-press Select starts multi-word range (only after opening long-press is released)
- Docs: `docs/dictionary.md`

### KOReader Sync
- `lib/KOReaderSync/`
- Reader + settings activities

### Reader polish
- Side long-press Ignore ignores held releases
- Reader battery top-right

## Build

```bat
cd /d E:\casper
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -e default
```

Output: `.pio\build\default\firmware-default.bin`

## Remotes

```bat
git remote -v
rem crossink-local  -> C:\Users\m\CrossInk
rem crossink        -> https://github.com/uxjulia/CrossInk.git
rem upstream        -> https://github.com/crosspoint-reader/crosspoint-reader.git
```

## Do not commit

- `.pio/`
- `scripts/data/`
- `docs/*.cxdict` (except sample)
- `platformio.local.ini` / secrets
