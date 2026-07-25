---
title: Casper tour
nav_order: 2
description: Visual tour of Casper home, reader, stats, controls, and dictionary
---

# Casper tour

**Casper** is personal firmware for the **Xteink X3/X4**, built on
[CrossPoint Reader 1.5](https://github.com/crosspoint-reader/crosspoint-reader).
It keeps a solid EPUB/TXT reading core, then layers Casper branding and a
reading-first UI: Dashboard home, redesigned reader chrome, reading stats,
flexible controls, and offline StarDict dictionary lookup with multi-word
selection. (KOReader Sync is **not** part of the current build.)

[View on GitHub](https://github.com/tweakerinc/casper) ·
[Releases](https://github.com/tweakerinc/casper/releases) ·
[Installation](./installation.md)

---

## Dashboard home

Current book cover, live metrics, lifetime totals, streak, and short front-button
labels — **Menu · Browse · Settings · Read**.

<img src="./images/casper/dashboard.jpg" alt="Casper Dashboard home with book cover and stats" width="360" />

*Dashboard — cover, book metrics, lifetime grid, 16-day streak, Night Reader.*

---

## Reading view

Chrome stays small so the page stays the focus: battery, clock, progress,
time left, chapter, and page number.

<img src="./images/casper/reading-view.jpg" alt="Casper reading view with status bar chrome" width="360" />

*Reader — top battery / clock / percent complete; bottom time left, chapter, Pg. n/m.*

---

## Reading stats

Full **Reading Stats** for the current book, plus device lifetime charts.

<img src="./images/casper/reading-stats.jpg" alt="Per-book reading stats with time of day and day of week charts" width="360" />

*Per-book stats — sessions, time, progress, pace, start / est. finish, habit charts.*

<img src="./images/casper/lifetime-stats.jpg" alt="Device lifetime reading stats with charts" width="360" />

*This device — lifetime totals plus time-of-day and day-of-week breakdowns.*

---

## Controls

Defaults tuned for everyday use: short power sleeps, long power refreshes,
long-press menu opens the dictionary.

<img src="./images/casper/controls.jpg" alt="Settings Controls tab with power and menu defaults" width="360" />

*Settings → Controls — Short-Press Power = Sleep, Long-Press Menu = Dictionary.*

---

## Dictionary

Offline **StarDict** folders under `/dictionaries/` (one folder per pack). Multi-select
which packs cascade on lookup. Install notes: [Dictionary](./dictionary.md).

### Pack selection

<img src="./images/casper/dictionary-settings.jpg" alt="Dictionary pack multi-select screen" width="360" />

*Dictionary settings — enable English, English–Spanish, Spanish–English packs.*

### English + bilingual cascade

One lookup can show English senses and a bilingual gloss in the same card,
with pronunciation beside the headword.

<img src="./images/casper/english-definition.jpg" alt="Dictionary card for transformation with English and English-Spanish" width="360" />

*`transformation` — English definition plus English–Spanish gloss.*

### Spanish clitics

Forms like **ayúdame** resolve to the verb stem with a natural “help me” line.

<img src="./images/casper/spanish-english.jpg" alt="Dictionary card for ayudar from Spanish clitic ayudame" width="360" />

*`Ayúdame` → ayudar / help me (Spanish–English).*

### Multi-word selection

Long-press **Select** to arm a range, move with Left/Right, short **Select** to
look up the phrase (e.g. collocations like `por favor`).

<img src="./images/casper/multi-word-selection.jpg" alt="Multi-word selection of por favor with definition please" width="360" />

*Multi-word selection — `por favor` → “please”.*

---

## Highlights at a glance

- **Casper** branding (boot, portal, serial / **v0.1**)
- **Dashboard** home + quiet reader chrome
- **Reading stats** per book and this device (with charts)
- **Controls** defaults: sleep / refresh / dictionary
- **Offline StarDict dictionary** with multi-pack cascade and multi-word selection
- **Not current:** KOReader Sync

---

## Get the firmware

1. Open **[Releases](https://github.com/tweakerinc/casper/releases)**  
2. Download the `.bin` for your build  
3. Follow **[Installation](./installation.md)**  

Upstream project and community support:
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
