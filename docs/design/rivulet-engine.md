# Rivulet Engine — Content Architecture Charter

**Status:** Implementation started (`lib/Rivulet/`)  
**Product:** Casper e-reader (ESP32-C3)  
**Priorities:** Speed → RAM → Stability → Performance  
**Script focus:** Latin first; RTL / auto page-turn / full BiDi out of v1 scope  

## Goals

A **book-class** EPUB content engine that:

1. Preserves publisher look that readers feel: **indent, alignment, headings, bold/italic, drop caps, real glyphs**.
2. Uses **Tier A + B only** (no Tier C fat page-bin product design).
3. Fits **~380 KB heap** with a bounded working set.
4. Keeps **chapter and book page counters** via thin page maps (exact after layout pass; estimate until then).
5. Is **clean, small, and replaceable** — not a style layer bolted onto CrossPoint page dumps.

## Non-goals (v1)

- Right-to-left / full BiDi
- Auto page turning
- Mouse-tail progressive poems (deferred; not forbidden by architecture)
- Greyscale multipass as part of the engine
- Full CSSOM / `::first-letter` pseudo engine
- Nested tables, absolute positioning, multi-column

## Tiers

| Tier | What | When |
|------|------|------|
| **A** | Stream / IR → layout **current page + 1 ahead** only | Every paint |
| **B** | Chapter **IR** on SD (`*.rvir`) | First visit or idle convert |
| **Maps** | Thin page → (block, run, byte) index | After full chapter layout or progressive fill |
| **C** | Full serialized page paint caches | **Not a product pillar** |

## Styling subset (must work)

- Bold / italic / bold+italic  
- Size steps relative to user base (heading ladder + CSS relative)  
- Text align: left / center / right / justify  
- First-line indent (em-based)  
- Block margins (simplified)  
- Drop cap (metric 2–4× NN scale, 2-line wrap zone, page-break if insufficient room)  
- System fonts first: **Source Serif 4** and **Literata** (glyph + drop-cap validation)  

## Pipeline

```
EPUB XHTML bytes
  → HtmlToIr (stream, Latin tags)
  → ChapterIr (RAM) + save .rvir (SD)
  → PageLayouter (one page window)
  → paint via GfxRenderer
  → PageMap (optional full pass / idle)
```

## Integration

`lib/Rivulet` is standalone from `lib/Epub` Section/TextBlock.  
Casper reader activities will switch over behind a flag after the engine proves open/page/drop-cap on device.

## Version

IR magic `RVIR`, format version in `IrFormat.h` — bump when on-disk layout changes.
