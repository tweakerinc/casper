# Rivulet vs classic EpubReader — parity gap

**Status:** Audit after premature `CASPER_RIVULET_READER=1` swap  
**Classic size:** ~4k lines `EpubReaderActivity` + Section/TextBlock/CSS pipeline  
**Rivulet shell:** ~700 lines `RivuletReaderActivity` + ~60KB `lib/Rivulet` engine  

The eval swap replaced **page paint** only. The product reader is a large activity surface
(menu, stats, progress, bookmarks, clippings, footnotes, KOSync, end-of-book, ghosting,
image chapters, CSS, …). Those were not ported systematically.

## Principle for next work

1. **Classic remains default** until a written parity checklist passes on device.
2. Rivulet is an **engine** (IR + layout + paint). Product behavior lives in the **activity shell**.
3. Prefer **reuse** of existing activities (`EpubReaderMenu*`, bookmarks, KOSync, stats) with
   thin adapters (spine/page/progress callbacks) — not reimplementation.
4. Ship in **tiers** (P0–P3 below). Do not claim “reader swap” until P0 + P1 are done.

---

## Feature matrix

| Area | Classic EpubReader | Rivulet today | Gap |
|------|--------------------|---------------|-----|
| **Open / load** | book.bin, CSS, section caches, FB loan, first-open land | book.bin, skip CSS, IR convert, FB loan, land + progress.bin | CSS styles ignored by design for v1; section cache unused |
| **Page paint** | Section/TextBlock + CSS resolve + images | IR PageLayouter + glyph spans | Images, CSS, tables, footnotes marks incomplete |
| **Page turn** | PageBack/Forward, latch, tilt, power, touch, chapter hop | Same latch path (recently wired) | Long-press menu shortcuts incomplete |
| **Confirm menu** | Full menu + long/double-press shortcuts | Opens menu; few actions work | Most actions stubbed |
| **Select chapter** | TOC + anchor resume | TOC → spine start only | Anchor within chapter missing |
| **Go to %** | Percent UI + spine/page map | Stub | Full missing |
| **progress.bin** | Load/save spine+page+count | Load/save (recent) | Page indices differ from classic pagination |
| **Home progress ring** | stats_v6 + Penumbra cache | Recents add only | `persistHomeProgressPercent` missing |
| **Reading stats** | Session clock, pace, time-left, complete stamp | None | Full missing |
| **Bookmarks** | Add/toggle/list/delete, % based | Menu stub | Full missing |
| **Clippings** | Select, list, jump, highlights | Menu stub | Full missing |
| **Dictionary** | Word select + definition | Menu stub | Full missing |
| **Footnotes** | Per-page list, href nav, stack | None | Full missing |
| **KOReader sync** | Upload/download, leave gates | Menu stub | Full missing |
| **Auto page turn** | Timed turns | Menu may set option; no loop | Loop missing |
| **BT page turner** | Quick connect child | Stub | Full missing |
| **Orientation** | Apply + reflow | Apply + goToStart | OK-ish; reflow key change needs re-layout |
| **Text settings** | Full UI + reflow fingerprint | Opens TextSettings; re-layout start | SD fonts ladder limited |
| **Status bar** | Progress, titles, time-left, bookmark, estimates | drawStatusBar basic | Time-left, bookmark flag, accurate book pages |
| **Chrome insets** | Top chrome extra + bottom status reserve | Matched Epub (recent fix) | Verify on device |
| **Anti-ghosting** | displayWithRefreshCycle + greys AA | Cycle only | Text AA multipass missing |
| **Images** | PNG/JPG plates, greys, cache | None (text-only IR) | Full missing |
| **Drop caps** | Section + CSS + NN scale | IR flag + ladder scale | Quality still WIP |
| **Hyphenation / bionic / guide dots** | Settings-driven | Not in layouter | Missing |
| **Justify / alignment** | CSS + settings | Partial flags | Weak |
| **End of book** | Screen + next-book suggestions + finished folder | Popup only | Full missing |
| **Screenshot** | Capture + metadata | Metadata only | Capture path? |
| **QR display** | Child activity | Stub | Missing |
| **Delete cache** | Book cache wipe policy | Rivulet IR dir only | Partial |
| **Delete stats / reset pace** | Full | Stub | Missing |
| **Mark finished** | Stats + folder move | Stub | Missing |
| **Sleep / QR sticky path** | openEpubPath + loadCount | Sets openEpubPath | Verify QR resume |
| **Forced refresh** | Yes | Yes | OK |
| **Idle prewarm / neighbor build** | Yes | No | Missing (perf) |
| **Leave “Saving…” chrome** | Yes | No | Missing |

---

## Menu action inventory

| MenuAction | Rivulet |
|------------|---------|
| GO_HOME | Works |
| SELECT_CHAPTER | Works (spine start; no anchor) |
| MANAGE_FONTS | Opens TextSettings |
| DELETE_CACHE | Clears `…/rivulet/` only |
| ROTATE_SCREEN / ORIENT_FRONT_BUTTONS | Applied in callback |
| DICTIONARY | Stub |
| FOOTNOTES | Stub |
| GO_TO_PERCENT | Stub |
| AUTO_PAGE_TURN | Stub (option not driven) |
| BLUETOOTH | Stub |
| READING_STATS | Stub |
| TOGGLE_COMPLETED | Stub |
| SYNC | Stub |
| TOGGLE_BOOKMARK / BOOKMARKS / DELETE_BOOKMARKS | Stub |
| SAVE_CLIPPING / VIEW_CLIPPINGS | Stub |
| SCREENSHOT | Stub |
| DISPLAY_QR | Stub |
| DELETE_STATS / RESET_READING_PACE | Stub |

---

## Engine vs shell (what belongs where)

| Layer | Owns |
|-------|------|
| `lib/Rivulet` | HTML→IR, layout window, page map, paint spans, drop-cap metrics |
| Activity shell | Progress, stats, menu wiring, bookmarks, KOSync, end-of-book, chrome, input policy, SD side-effects |
| `lib/Epub` (keep) | Zip, OPF, TOC, book.bin, CSS (optional later), images |

Rivulet should **not** reimplement KOSync/bookmarks/stats formats. It should expose:
`spineIndex`, `page`, `chapterPageCount`, `bookProgress01()`, `goToSpinePage()`, `reloadAfterSettings()`.

---

## Recommended ship tiers

### P0 — Product usable for daily reading (must before default-on)
- Classic **default** again (`CASPER_RIVULET_READER` off)
- Chrome insets verified (no text under status bar)
- progress.bin load/save + home ring % (`persistHomeProgressPercent` / stats_v6 minimal)
- Menu: Go home, chapter, %, text settings, orientation, delete cache, screenshot, force refresh
- Page turn parity (already close) + chapter skip without freeze
- End-of-book screen (reuse `EndOfBookOptions` if possible)

### P1 — “Feels like Casper”
- Bookmarks (reuse list activities + % based entries)
- Reading stats session on enter/exit
- Go to percent (map via `Epub::calculateProgress` inverse)
- Auto page turn loop
- Text AA optional path or document as deferred
- Hyphenation/bionic/guide if settings on (or hide settings that no-op)

### P2 — Content fidelity
- Images as blocks (or placeholder skip without crashing)
- CSS-driven drop-cap / indent / align improvements
- Footnote marks + FOOTNOTES menu
- Anchor jump within chapter

### P3 — Sync & extras
- KOReader sync
- Clippings
- Dictionary
- BT page turner
- QR, finished-folder move, leave-sync gates

### Ownership pillars (stable id / ledger / `/.casper` files)
**Not a blocker for P0–P1.**  
Use existing `epub_*` cache + `progress.bin` + `stats_v6` until product is usable.  
Ownership work is for content-stable identity and cleaner file layout **after** shell parity.

---

## Process correction

| Wrong | Right |
|-------|--------|
| Flip flag → “new reader” | Flag only behind parity checklist |
| Stub menu with “Not in Rivulet yet” as normal | Either wire or hide unavailable items |
| Reimplement product features in engine | Adapter shell + shared activities |
| Estimate last-page → full layout walk | Never block UI without budget/yield |

---

## Immediate code state (this branch)

- `CASPER_RIVULET_READER` should be **0 / unset** for daily builds.
- Keep `RivuletReaderActivity` + `lib/Rivulet` for development.
- Resume eval with `-DCASPER_RIVULET_READER=1` only for focused tests.

## Acceptance for “Rivulet is the reader”

On Butcher / Alice / Dune class books:

1. Open, resume, page, chapter, %, back home without freeze/crash  
2. Status bar and margins match classic chrome  
3. Home progress ring updates after reading  
4. Bookmarks + stats work  
5. Menu has no dead primary actions (or actions hidden)  
6. Drop caps + body text within viewport for SS4 + Literata  

Until then: **classic path is production.**
