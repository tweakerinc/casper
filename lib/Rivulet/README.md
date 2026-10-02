# Rivulet Engine

Clean-room **EPUB content engine** for CrossPoint: book-class styling with a RAM-first architecture.

## Priorities

1. Speed  
2. RAM  
3. Stability  
4. Overall performance  

## Architecture (Tier A + B)

| Tier | Role |
|------|------|
| **A** | Layout **one page** (+ optional next) from chapter IR — never whole-chapter geometry in RAM |
| **B** | Persist chapter IR as `*.rvir` on SD after convert |
| **Maps** | Thin `*.rvpm` page → cursor tables for counters / jumps |
| **C** | Fat page paint caches — **not used** |

## What v1 supports

- Latin text  
- **Source Serif 4** + **Literata** size ladders  
- Bold / italic  
- Headings h1–h6 (size + center)  
- Paragraph indent, align (left/center/right/justify)  
- Drop caps (metric 2–4×, top-of-page preference)  
- HR  
- Real glyphs via `GfxRenderer`  

## Out of v1 scope

- RTL / full BiDi  
- Auto page-turn  
- Mouse-tail poems (architecture allows later)  
- Greyscale multipass (UI/reader shell concern)  

## API sketch

```cpp
rivulet::RivuletEngine eng;
rivulet::RenderKey key;
key.fontId = SOURCESERIF4_12_FONT_ID; // or Literata
key.viewportW = 480;
key.viewportH = 700;
eng.setRenderKey(key);

eng.ingestHtml(xhtmlBytes, len, "/.crosspoint/book/ch5.rvir", /*dropCapFirst*/ false);
eng.goToStart(renderer);
eng.paint(renderer, marginX, marginY);

// Idle:
eng.buildPageMap(renderer);
eng.savePageMap("/.crosspoint/book/ch5.rvpm");
```

## Files

| File | Role |
|------|------|
| `IrFormat.h` | Magic, versions, enums |
| `ChapterIr.*` | In-memory IR + SD load/save |
| `HtmlToIr.*` | XHTML subset → IR |
| `PageLayouter.*` | Tier A layout |
| `PageMap.*` | Thin page index |
| `FontLadder.*` | SS4 / Literata resolve |
| `RivuletEngine.*` | Facade |

## Integration (live test path)

`CROSSPOINT_RIVULET_READER=1` in `platformio.ini` routes EPUB opens to
`RivuletReaderActivity` (minimal chrome: page turns, chapter advance, IR on SD).

- **1** = Rivulet engine paints pages (for second-device eval)  
- **0** = classic CrossPoint `EpubReaderActivity` / `Section` path  

Both codepaths remain in the firmware until Rivulet is proven; flash size is not reduced yet.

## Design charter

See `docs/design/rivulet-engine.md`.
