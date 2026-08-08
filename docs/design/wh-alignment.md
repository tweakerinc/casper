# Witchhunt alignment map

Casper treats `_xref_witchhunt` as the **behavioral source of truth** for EPUB
layout quality and background indexing discipline. Ports are surgical: keep
Casper BiDi, poem margins, lock budget, BW-first greys, glyph grey-skip, and
partial suspend.

## Source → target

| Witchhunt | Casper |
|-----------|--------|
| `ChapterHtmlSlimParser` float/drop-cap | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.*` |
| `ParsedText` float-aware breaks | `lib/Epub/Epub/ParsedText.*` |
| `FontSizeLadder.h` | Merge with `css/StyleResolve.*` (phase 3) |
| `ImageBlock` dual cache / warm | `blocks/ImageBlock.*` (phase 1: no FB stamp) |
| `Page` warm + grey replay | `Page.*` |
| `Section` stepSectionBuild / B gates | `Section.*` + neighbor build |
| `EpubReaderActivity` A/B/C service | `src/activities/reader/EpubReaderActivity.*` |

## Phase status

| Phase | Status |
|-------|--------|
| 1 Paint thrash (precache stamp, font clear, image gate, glyph greys) | **In progress** |
| 2 Float + drop-cap algorithms | Pending |
| 3 CSS size / #id | Pending |
| 4 Neighbor WaitHeap discipline | Partial (gates started) |
| 5 Dual cache / Background A / tables | Later |

## Golden books

- Alice (figleft letter, Tenniel, mouse-tail, ch. III–IV)
- God Emperor ch.1 (text drop-cap, epigraph)
- One plain novel

## Rules

1. Prefer porting a WH function over inventing Alice-only heuristics.
2. Every layout change names the WH symbol or file it ports.
3. No SECTION version bump without cache invalidation need.
4. Regression: Alice + God Emperor + plain novel.
