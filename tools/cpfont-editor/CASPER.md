# Casper notes — iwalton3/cpfont-editor

Cloned from https://github.com/iwalton3/cpfont-editor

## What it does

- Builds **`.cpfont` SD-card fonts** (not the firmware’s built-in `.h` headers).
- **Stem calibration** + **gap-fix**: rasterizes small sizes at a ppem where stems hit whole pixels → cleaner on X3/X4 e-ink.
- Pixel editor / nudge tools for hand-tuning.

## Casper Literata (incl. 10 pt)

`sd-fonts.yaml` Literata `sizes` includes **10** (Casper reader ladder uses 10/12/14/16).

```bat
cd tools\cpfont-editor\build
pip install freetype-py fonttools pyyaml
python build-sd-fonts.py --only Literata --stem-calibrate --output-dir .\output-literata-casper
```

Output: `output-literata-casper/Literata/Literata_{10,12,14,16,18}.cpfont`

### Install on device (SD)

```text
/.fonts/Literata/Literata_10.cpfont
/.fonts/Literata/Literata_12.cpfont
...
```

Then Settings → font family → **Literata** (SD entry). That uses these packs instead of the flash-built Literata.

### Rebuild only size 10 after YAML already has 10

Same command as above; it regenerates all listed sizes.

## Flash vs SD

| | Built-in (firmware) | These `.cpfont` packs |
|--|---------------------|------------------------|
| Format | `literata_*.h` in flash | SD only |
| Rebuild builtins | `lib/EpdFont/scripts/build-literata-builtin.py` | `build-sd-fonts.py` |
| Appearance | **stem-calibrated ppem + gap-fix** (ported into `fontconvert.py`) | same ideas |

### Rebuild flash Literata (firmware base)

```bat
cd lib\EpdFont\scripts
python build-literata-builtin.py
```

Then update `src/fontIds.h` IDs (SHA256 of the four style headers per size) and the Literata ladders in `StyleResolve.cpp` / `FontLadder.cpp`.

## Line metrics note

Stem-calibrated Literata **10** reports `advanceY≈33` (dense). Slot **12** can still be taller after ppem nudge — line compression still applies on top.
