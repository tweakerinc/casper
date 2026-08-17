# Nova roadmap

## Phase 0 — Skeleton (this commit)

- Boot, SD, display, fonts
- Activity stack
- Home (Penumbra/Bare chrome)
- File browser → open EPUB
- Rivulet reader: page next/prev, progress under `/.casper/book_<id>/`
- Settings: font size, margins, refresh interval, theme, sleep mode, button map (basic)
- Sleep frame path under `/.casper`

## Phase 1 — Reader product

- Chapter list / TOC
- Status bar (page, %, clock)
- Idle page-map fill
- Dictionary (optional, later)
- Bookmarks / clippings under book folder

## Phase 2 — Network modes

- Wi‑Fi STA credentials store
- File transfer web UI (upload EPUB)
- OPDS browser (catalog + download)
- Explicit “exit network mode” to reclaim heap before reading

## Phase 3 — Polish

- Quick resume / sleep art
- Anti-ghost defaults tuned
- OTA from GitHub releases
- Drop dependency on any remaining shared CrossPoint-era helpers if still present

## Explicit non-goals (v1)

- Classic EPUB layout engine
- Multi-language UI
- RTL / CJK
- Auto page turn
- KOReader sync
- Theme zoo beyond Penumbra + Bare
