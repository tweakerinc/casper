# CLAUDE.md — CrossPoint Font Tools

Guidance for AI agents working in this repository.

## What this is

Host-side (desktop Python) tooling to build and hand-tune the `.cpfont` SD-card
fonts for [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader),
an ESP32-C3 e-reader (Xteink X3/X4). **This is not firmware** — none of it runs
on the device, so the firmware's RAM/flash constraints do not apply here. Output
`.cpfont` files are consumed by the device unchanged.

Two parts:
- `build/` — a self-contained copy of CrossPoint's font build pipeline, plus a
  **stem calibration** feature.
- top level — a local web glyph editor and analysis helpers.

## Provenance — important

The files in `build/` (`build-sd-fonts.py`, `fontconvert_sdcard.py`,
`cpfont_version.py`, `generate-font-manifest.py`, `sd-fonts.yaml`) and the
`.cpfont` format come from the upstream CrossPoint Reader project (MIT, see
`LICENSE`). Do **not** present them as original work, and keep changes to them
minimal and clearly scoped (currently: the stem-calibration additions only).
The editor tools and the calibration logic are the original contribution.

Two paths in `build/build-sd-fonts.py` were repatched for self-containment
(`DEFAULT_FALLBACK_FONT`, `manifest_script`) and `sd-fonts.yaml`'s
NotoSansExtended `path:` entries point at `build/fallback-fonts/`. Preserve those
when syncing from upstream.

## The core idea (stem calibration)

A near-1-bit e-ink panel renders a font cleanly only at ppems where its stem
width is a **whole pixel** (2/3/4px). Between them the stem gets a gray fringe
that looks muddy on the low-PPI X4. Fix: rasterize a size *slot* at the ppem
where the stem is a clean whole pixel, not at the nominal point size. The
filename keeps the slot label (`_12`), only the render ppem changes. The width is
chosen as the clean width whose ppem is **nearest the nominal size** (NOT always
2px) — thin faces get 2px, but heavier/serif faces get 3px instead of being
shrunk until their serifs break (NotoSerif's 2px point is 8.6pt/ragged; 3px is
~14pt/clean). By default only Small (≤12pt) is calibrated; `--calibrate-max-size`
widens that.

Settled conclusions (don't relitigate without new device evidence):
- **Keep 2-bit anti-aliasing.** 1-bit MONO was tested on device and looked worse.
- Don't auto-"crispen" stem fringes in the bitmap — a bowl edge is
  indistinguishable from a stem at ~12px; it over-reaches. The safe automated op
  is gap-fill only (`destem.crispen(..., gaps_only=True)`), which is now ALWAYS
  applied in the build (`--no-gap-fix` to disable). It re-opens muddy 1px gaps
  (the `"` quote blob) and touches nothing else.
- Calibration is **size-neutral** with the small-only cap; the only size growth
  vs older sets is the NotoSans fallback adding glyph coverage.
- **Auto-calibration is suggestion-grade, not authoritative** — "cleanest stem"
  isn't always "looks best" (serifs chasing 2px end up too small). The preferred
  workflow is the **nudge preview**: `nudge_preview.py` renders each font's
  pangram at a range of ppems (gap-fixed), the human picks per font, exports
  `picks.json` (`{family:{size:ppem}}`), and the build consumes it via
  `--ppem-config` (wins over `--stem-calibrate`; unlisted sizes = nominal).
  The build default is now **nominal + gap-fix** (calibration is opt-in).
- **The suggestion heuristic** (`suggest_ppem`): within **±1.5pt** of nominal,
  pick a ppem by an objective that DISPATCHES on the font's stroke consistency.
  Consistency is measured by stroke **contrast** = the `o` side(thick)/top(thin)
  ratio at large ppem (`_stroke_contrast`); `< CONTRAST_THRESHOLD` (1.75) =
  *monolinear*, else *variable*. No serif/sans tags — contrast is the real
  property (slab serifs read monolinear, modulated serifs read variable). Two
  branches:
  - **monolinear** (sans, slab): target a clean integer stem width — the nearest
    integer to the font's natural stem at that size (2px at Small, ~3px at
    Medium), so a slot keeps its intended size. Objective `(|cov-target|+1.5·gray)·STEM_2PX_W`.
  - **variable** (high contrast): minimize **vertical light-grey fringe columns**
    (a column whose level-1 run exceeds `FRINGE_MIN_RUN`=4 is a fringed stem edge;
    dark grey = ink, not counted). Avoids the ragged-2px serif problem.
  Both add a **stem-unevenness** penalty (`UNEVEN_W`) and a small size-deviation
  tiebreak (`SUGGEST_SIZE_W`).
- **The irreducible residual:** when two ppems are equally clean by every stem
  metric (e.g. Lexica 22/23/24 are all cov 2.00, 2% gray, 0 unevenness), the
  pick between them is a holistic SHAPE judgment (the `u`) that NO scalar metric
  captures — that's the nudge step's job, not the heuristic's. Lexica's Small is
  pinned to ppem23 via `picks-overrides.json` for exactly this reason.

## Invariants — do not break

- **`.cpfont` rebuild must round-trip byte-for-byte.** `python3 cpfont_engine.py`
  asserts `parse → _serialize()` reproduces the input exactly, and that the 2-bit
  bitmap codec round-trips. Run it after any change to the engine.
- The 2-bit pixel quantization (`v>>4`, levels at 4/8/12) must stay identical
  across `fontconvert_sdcard.py`, `cpfont_engine.py`, `cpfont_stem_analyze.py`,
  and `destem.py`. The calibration stem-measurement (`_measure_stem`) MUST
  quantize the same way — measuring 8-bit coverage directly gives wildly wrong
  (pessimistic) numbers.
- Editing a glyph changes only its bitmap + metadata; intervals, kerning and
  ligature sections are preserved opaque.

## How to test

```bash
python3 cpfont_engine.py                       # roundtrip + resize self-test
cd build && python3 build-sd-fonts.py --only LexicaUltralegible \
  --output-dir /tmp/t --verbose                # end-to-end build
# verify cleanliness of a result:
python3 -c "import cpfont_stem_analyze as A; print(A.analyze_file('/tmp/t/LexicaUltralegible/LexicaUltralegible_12.cpfont')['grayLoad'])"
```

Deps: `freetype-py`, `fonttools`, `pyyaml`. Stdlib-only for the editor runtime
(`glyph_editor.py`, `cpfont_engine.py`); FreeType/fontTools only for building.

## Conventions

- Python, standard library first; avoid adding dependencies (the three above are
  the full set). Match the existing style of each file.
- Keep the editor tools stdlib-only so they run anywhere with just a browser.
- When in doubt about a device-visible rendering change, prefer measuring
  (`cpfont_stem_analyze`, ASCII dumps) over asserting; the whole point of this
  toolkit is that small-size rendering is unintuitive.
