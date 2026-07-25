---
title: Reader Features
nav_order: 17
---

# Reader Features

Casper is built on **CrossPoint Reader 1.5** with Casper UI and dictionary work on top. This page is a practical overview of reader-related behavior. Some older docs in this repo still describe **CrossInk-era** experiments; where those are **not** part of the current Casper build, they are marked below.

For release notes as published, see [Releases](https://github.com/TweakerInc/casper/releases). For screenshots, see [Casper tour](./casper.md).

---

## In-book and Settings options

Reader-facing options live under **Settings → Reader** (and related menus), for example:

- Font family / size  
- Line spacing  
- Margins  
- Paragraph alignment (Casper default: **left**)  
- Image rendering  
- Embedded book styles  
- Hyphenation / Focus Reading (when present in the build)

Changes apply on the next layout pass for the book.

For books that are slow to index or fail because of complex publisher styling, see [EPUB Render Modes](./epub-render-modes.md) if that page still applies to your build.

---

## Fonts

Casper aims for comfortable e-ink type (Lexend Deca / Bitter in builds that include those packs; otherwise the firmware’s built-in families). SD-card font packs may still be supported via CrossPoint’s SD font system — see [SD Card Fonts](./sd-card-fonts.md) and [Font Build Variants](./font-build-variants.md).

---

## Time left and status bar

The reader can show estimated time left **in book** or **in chapter**, plus battery, clock (when RTC is present), book progress %, chapter page counts (`Pg. n/m`), and an optional thin progress bar. Casper defaults lean toward book-scoped time left and a thin book progress bar.

Pace is learned from normal forward page turns. Use any **Reset reading pace** control in settings/stats if the estimate was trained by unusual navigation.

---

## Bookmarks

EPUB bookmarks from the reader (add, list, jump, delete) come from the CrossPoint reader stack when enabled in your build.

---

## Reading stats

Casper tracks **per-book** and **device lifetime** stats (sessions, time, progress, pace, streak, habit-style charts on supported screens). Open **Reading Stats** from the long-press menu (when assigned) or related home/Dashboard entry points.

- Date-related detail needs a real-time clock (typical X3). X4 without RTC has a thinner clock/date story.  
- Clear-cache tools should preserve progress/stats files when implemented as in current Casper.

### CrossInk-only / not current Casper

| Feature | Status |
|---------|--------|
| **KOReader Sync** | Supported (1.5 Sign Up/Authenticate + Casper auto-upload). See [KOReader Sync](./koreader-sync.md) |
| Nearby Position Sync (ESP-NOW) | CrossInk-era doc; not a Casper 1.5 highlight |
| Nearby Reading Stats Sync | CrossInk-era doc; treat as optional/upstream only |
| Dark Reader Mode, Guide Dots, Bionic/Focus Reading as product pillars | May exist upstream or partially; not the focus of the Casper photo tour |
| Clippings / My Clippings.txt | CrossInk-era pipeline; confirm on your build before depending on it |
| Stats-as-sleep-screen modes | Not a current Casper claim |

Older pages such as [Reading Stats Sync](./reading-stats-sync.md) and [Nearby Position Sync](./nearby-position-sync.md) remain for historical/upstream reference.

---

## Finished books and Read folder

You can mark a book finished from the reader/menu when those actions are present. Optional **move finished books to `/Read/`** and **remove from recents** are system/library settings when enabled (default off in Casper factory settings).

---

## Reader controls and shortcuts

Casper factory control defaults:

- Short power → **Sleep**  
- Long power → **Force refresh**  
- Long-press Menu → **Dictionary**  
- Side long-press → **Off**

Remapping and related options: [Controls](./controls.md).
