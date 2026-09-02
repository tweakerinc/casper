# CrossPoint Font Tools

Tooling to build and hand-tune the `.cpfont` SD-card fonts used by
**[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)**,
the open-source e-reader firmware for the Xteink X3 / X4.

To see a preview of the fonts, [go here](./font_example_images.png).

It does two things:

1. **`build/`** — a self-contained copy of CrossPoint's font build pipeline,
   extended with **stem calibration**: it renders each small size at the ppem
   where the font's stems land on a whole pixel, so text is crisp on the
   low-PPI X4 instead of muddy.
2. **the editor tools** (top level) — a local web pixel-editor and helpers for
   inspecting and fixing individual glyphs that still need a human touch.

> **Credit.** The font build scripts (`build-sd-fonts.py`, `fontconvert_sdcard.py`,
> `cpfont_version.py`, `generate-font-manifest.py`, `sd-fonts.yaml`) and the
> `.cpfont` format originate from the CrossPoint Reader project:
> <https://github.com/crosspoint-reader/crosspoint-reader>. They are included
> here under that project's MIT license (see `LICENSE`). The bundled NotoSans
> fallback fonts are licensed under the SIL Open Font License
> (`build/fallback-fonts/OFL.txt`). The stem-calibration additions and the
> editor tools in this directory are the only original work here.

---

## Why stem calibration

A near-1-bit e-ink panel only renders a font *cleanly* at the sizes where its
stem width lands on a **whole pixel** (2px, 3px, ...). Between them — e.g. a
2.4px stem — the stem picks up a gray anti-aliasing fringe that reads as muddy
on the low-PPI X4, and makes letters like `I`, `g`, `"` look strange.

The fix isn't the font or hinting — it's the *size*. The standard slots
(Small=12pt, Medium=14pt at 150 DPI) often fall in a font's muddy valley.
Calibration rasterizes the slot at the nearby ppem where the stem is a clean
**whole pixel** (2px, 3px, ...), keeping the same `_12` filename. It picks the
clean width whose ppem is *nearest the nominal size* — so thin faces land on a
crisp 2px near 12pt, while heavier/serif faces (whose 2px point would be far too
small) land on 3px instead. Without this, NotoSerif's only 2px point is 8.6pt,
where its serifs render ragged; nearest-to-nominal gives it a clean 3px at ~14pt.

By default **only Small** is calibrated (at larger sizes the fringe is a small
fraction of a big stem and reads as texture, not the focal point).

See `git log` / the CrossPoint repo for the full investigation; the short
version: keep 2-bit anti-aliasing (1-bit MONO tested worse on device), and fix
small-size muddiness by calibrating the render ppem.

---

## Building the fonts

Requires Python with `freetype-py`, `fonttools`, and `pyyaml`
(`pip install freetype-py fonttools pyyaml`). The build downloads font sources
over the network and instances variable fonts automatically. The **gap-fix**
correction (re-opening muddy 1px gaps like the `"` quote) is always applied.

### The default: nominal sizes + gap-fix

```bash
cd build
python3 build-sd-fonts.py --output-dir ~/BulkDocuments/crosspoint-fonts
```
Every size renders at its nominal point size with gap-fix. Limit with
`--only LexicaUltralegible,Inter`.

### The recommended path: nudge → pick → build

"Cleanest stem" isn't always "looks best" (a serif chasing a 2px stem ends up
too small). So the better workflow is to let your eye pick the render size per
font:

```bash
# 1. render each font's pangram at a range of ppems (gap-fixed) into a page
#    (reads sd-fonts.yaml; downloads + instances sources exactly like the build)
python3 ../nudge_preview.py --size 12 -o ../nudge.html

# 2. open nudge.html, click the least-strange size per font, "Export config" -> picks.json
#    (each font defaults to an auto-suggested ppem within +-1.5pt of nominal: monolinear
#     faces target a clean integer stem, high-contrast faces minimize light-grey fringe.
#     nudge from there -- some picks are aesthetic and below any metric's resolution.)

# 3. build using your picks
python3 build-sd-fonts.py --ppem-config ../picks.json \
  --output-dir ~/BulkDocuments/crosspoint-fonts-tuned
```
`picks.json` is `{family: {size: ppem}}`; sizes you don't pick render at nominal.
Filenames always keep the slot label (`_12`), only the render ppem changes.

### Auto-calibrated sets (the shortcut)

`--stem-calibrate` skips the hand-pick and auto-suggests each calibrated slot's
ppem — the same contrast-dispatched heuristic that seeds the nudge defaults
(monolinear faces → clean integer stem, high-contrast faces → least light-grey
fringe, within ±1.5pt). `--calibrate-max-size N` sets which slots are tuned
(≤ N pt). Per-font opt-out: `stem_calibrate: false` in `sd-fonts.yaml`.

```bash
cd build

# Small-only set — only the Small (12pt) slot is tuned; 14/16/18 stay nominal.
python3 build-sd-fonts.py --stem-calibrate \
  --output-dir ~/BulkDocuments/crosspoint-fonts-calibrated

# Medium set — Small AND Medium (12 + 14) tuned; 16/18 stay nominal.
python3 build-sd-fonts.py --stem-calibrate --calibrate-max-size 14 \
  --output-dir ~/BulkDocuments/crosspoint-fonts-calibrated-medium

# Overlay hand-picked overrides (e.g. Lexica Small pinned to ppem23 -- a clean-2px
# render the metric can't distinguish from 24, chosen by eye). Re-run per set:
python3 build-sd-fonts.py --only LexicaUltralegible --ppem-config ../picks-overrides.json \
  --output-dir ~/BulkDocuments/crosspoint-fonts-calibrated
```

All four styles of a font render at the one ppem chosen from its **regular**
style, so bold/italic stay the same size as regular. Add `--only A,B` to limit
families.

Copy a built family onto the SD card under `.fonts/<Family>/` to use it.
Filenames keep the slot label (`_12`); only the render ppem changes.

---

## The editor tools

Local, stdlib-only (plus a browser) tools for the glyphs that still need a hand.

| File | What it does |
|------|--------------|
| `glyph_editor.py` | Web pixel-editor: browse all glyphs as a grid, paint 2-bit pixels, **resize** the canvas, tweak advance/bearings, live word-context preview, save back to the `.cpfont`. |
| `cpfont_engine.py` | Parse / edit / **rebuild** `.cpfont` v4 (recomputes offsets, so resizes are safe). `parse → save` with no edits reproduces the input byte-for-byte. |
| `destem.py` | `crispen(levels, gaps_only=True)` — opens muddy blobbed gaps (e.g. the `"` quote) without touching anything else. The editor's "Gap-fix all" uses this. |
| `cpfont_stem_analyze.py` | Scores stem-width consistency / gray-edge load across a font's glyphs (no FreeType needed). |
| `rescale_cpfont.py` | One-off: build a `.cpfont` at a chosen **ppem**. `--find <ttf>` reports a font's clean ppems. Needs freetype + fonttools. |
| `generate_preview.py` | Render a side-by-side pangram preview of two font packs (original vs optimized) to a self-contained `preview.html`. |
| `nudge_preview.py` | Render each font's pangram at a range of ppems (gap-fixed) into an interactive page; click the least-strange size per font and export a `picks.json` the builder consumes. |

```bash
# visual before/after of the whole pack at Small (open preview.html in a browser)
python3 generate_preview.py \
  --original ~/BulkDocuments/crosspoint-fonts \
  --optimized ~/BulkDocuments/crosspoint-fonts-calibrated \
  --size 12 -o preview.html
```

```bash
# ALWAYS edit a copy — a rebuild would overwrite hand-edits.
cp ~/path/to/SD/.fonts/LexicaUltralegible/LexicaUltralegible_12.cpfont ./work.cpfont
python3 glyph_editor.py ./work.cpfont          # open http://127.0.0.1:8765/
```

Workflow: pick a glyph from the **grid** (filter/search supported) → **paint**
(keys `0`–`3` = white/light/dark/black, drag) while watching the `n o [glyph] o n`
context strip → **resize** the canvas with the arrow buttons (bearings
auto-adjust) or tweak `adv`/`left`/`top` → **Save**. `Gap-fix all (safe)`
un-muddies blobbed gaps across every glyph in one click.

Editing only changes the bitmap/metadata; intervals, kerning and ligatures are
preserved untouched.

---

## Layout

```
.
├── LICENSE                     MIT (CrossPoint Reader / Dave Allie)
├── cpfont_engine.py            editor: parse/edit/rebuild .cpfont
├── glyph_editor.py             editor: web UI
├── destem.py  cpfont_stem_analyze.py  rescale_cpfont.py
└── build/
    ├── build-sd-fonts.py       orchestrator (+ --calibrate-max-size)
    ├── fontconvert_sdcard.py   rasterizer (+ stem calibration)
    ├── cpfont_version.py  sd-fonts.yaml  generate-font-manifest.py
    └── fallback-fonts/         NotoSans (OFL) used to fill missing glyphs
```
