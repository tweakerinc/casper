# Casper

Personal firmware project based on CrossInk / CrossPoint Reader for Xteink X3/X4 (ESP32-C3).

This repo at `E:\casper` is the **Casper reference tree**: branding, defaults, dashboard, dictionary, KOReader sync improvements, and UI tweaks we want to keep when rebasing onto new CrossPoint releases.

## Why this folder exists

CrossPoint ships new firmware periodically. When that happens we want a clean merge path:

1. Take the **new CrossPoint (or CrossInk) base**
2. Re-apply **Casper deltas** from this tree (not a blind full overwrite)
3. Build and flash Casper

Use this project as the source of truth for “what is Casper,” not as the only place you ever edit.

| Path | Role |
|------|------|
| `C:\Users\m\CrossInk` | Day-to-day work / experiment tree (may drift) |
| `E:\casper` | Frozen-ish Casper reference + merge playbook |
| CrossPoint upstream | Fresh base when a release lands |

## Must-keep Casper features

### Branding
- Product name **Casper** (boot logo, serial, web titles, device name defaults, User-Agent)
- Boot logo assets under `src/images/` (`casper.png`, `Logo120.h`)
- Web chrome titles via `scripts/build_web.py` / `web/`

### Defaults (factory-ish settings)
Defined mainly in `src/CrossPointSettings.h`:

| Setting | Casper default | Intent |
|---------|----------------|--------|
| `shortPwrBtn` | `SLEEP` | Short power = sleep |
| `longPressMenuAction` | `LONG_MENU_DICTIONARY` | Long-press menu opens dictionary |
| `sideButtonLongPress` | `SIDE_LONG_OFF` | No accidental multi-page on side hold |
| Dashboard theme / sleep | available | Home dashboard UI |

Confirm these after any merge; upstream defaults often differ.

### Dashboard
- Theme: `src/components/themes/dashboard/DashboardTheme.cpp`
- Registration / metrics in `UITheme` + settings enums (`UI_THEME::DASHBOARD`, sleep modes)
- Cover thumbnail sizing / home layout in `HomeActivity*`

### Dictionary
- Library: `lib/Dictionary/`
- UI: `DictionarySelectionActivity`, `DictionaryLookupActivity`
- Reader entry: long-press menu / power shortcuts → dictionary selection
- Hyphen / multi-word lookup, top-right battery unrelated but often co-shipped
- Docs: `docs/dictionary.md`
- Packs: build with `scripts/build_en_dict.py` → install under `/.crosspoint/dict/` on SD

### KOReader Sync
- `lib/KOReaderSync/`
- `KOReaderSyncActivity`, `KOReaderSettingsActivity`, credential store
- Adaptive / furthest-ahead sync behavior and settings labels
- Defaults and web settings keys as customized in Casper

### Reader / controls UI tweaks
- Side long-press **Ignore** does not page-turn on held release (`ReaderUtils`, EPUB/XTC/TXT)
- Reader battery **top-right** (matches dashboard header)
- Dictionary selection: short select = word; long-press select = multi-word range
- Related status-bar / controls settings layout

## Build

```bat
cd /d E:\casper
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -e default
```

Output: `.pio\build\default\firmware-default.bin`

Simulator (if configured):

```bat
pio run -e simulator
```

## When new CrossPoint firmware lands

See **[docs/CASPER_MERGE.md](docs/CASPER_MERGE.md)** for the step-by-step rebase/merge checklist.

Quick version:

1. Add/fetch the new upstream tag or branch into a worktree
2. Diff this Casper tree against the pre-merge base to list deltas
3. Re-apply Casper patches feature-by-feature (branding → defaults → dictionary → KOReader → dashboard → controls)
4. Build `default`, flash, verify the checklist in `CASPER_MERGE.md`

## Git remotes (suggested)

```bat
cd /d E:\casper
git remote -v
rem crossink-local  -> C:\Users\m\CrossInk   (optional daily tree)
rem crossink        -> https://github.com/uxjulia/CrossInk.git
rem upstream        -> https://github.com/crosspoint-reader/crosspoint-reader.git
```

Point `upstream` at whatever official repo/tag you use for the new release.

## Do not commit

- `.pio/`
- `scripts/data/` (huge dictionary source dumps)
- `docs/*.cxdict` (built packs; keep sample only)
- `platformio.local.ini` / secrets

## Related docs

- `docs/CASPER_MERGE.md` — merge procedure
- `docs/dictionary.md` — dictionary format and packs
- `AGENTS.md` — firmware engineering rules (shared with CrossInk)
