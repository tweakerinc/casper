---
title: Casper tour
nav_order: 2
description: Visual tour of Casper themes, reader, stats, controls, and dictionary
---

# Casper tour

**Casper** is personal firmware for the **Xteink X3/X4**, built on
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
It keeps a solid EPUB/TXT reading core, then layers Casper branding and a
reading-first UI: **Bare** and **Stats** home themes, redesigned reader chrome,
reading stats, flexible controls, and offline dictionary lookup with multi-word
selection.

[View on GitHub](https://github.com/tweakerinc/casper) ·
[Releases](https://github.com/tweakerinc/casper/releases) ·
[Installation](./installation.md)

---

## Themes

Casper ships home skins for different readers.

### Bare · Stats

| Bare | Stats |
|:----:|:-----:|
| <img src="./images/casper/bare-theme.jpg" alt="Casper Bare theme home" width="280" /> | <img src="./images/casper/dashboard.jpg" alt="Casper Stats theme home" width="280" /> |

**Bare** focuses on the experience of just you and the book — a large cover,
clear title, and simple front-button actions (**Menu · Library · Synopsis · Read**)
without burying the page under metrics.

**Stats** is for power users and nerds who cannot get enough numbers — cover art
plus book metrics, lifetime totals, streak, and **Menu · Library · Settings · Read**.

### Synopsis view

From Bare (and the Synopsis action), open a short description of the current
book without diving into the file browser.

<img src="./images/casper/synopsis-view.jpg" alt="Casper book synopsis view" width="360" />

*Synopsis — title and blurb for the focused book.*

---

## Reading view

Chrome stays small so the page stays the focus: battery, clock, progress,
time left, chapter, and page number.

<img src="./images/casper/reader-ui.jpg" alt="Casper reading view with status bar chrome" width="360" />

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

Offline packs on the SD card. Multi-select which dictionaries cascade on lookup.

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

More install detail: [Dictionary](./dictionary.md).

---

## Highlights at a glance

- **Casper** branding (boot, portal, serial / **v0.1**)
- **Dashboard** home + quiet reader chrome
- **Reading stats** per book and this device (with charts)
- **Controls** defaults: sleep / refresh / dictionary
- **Offline dictionary** with multi-pack cascade and multi-word selection

---

## Get the firmware

1. Open **[Releases](https://github.com/tweakerinc/casper/releases)**  
2. Download the `.bin` for your build  
3. Follow **[Installation](./installation.md)**  

**On-device OTA:** after the first install, use **Settings → System → Check for updates**.
That hits this repo’s latest GitHub Release and installs a **`Casper-v0.1.0`** / **`Casper-v0.1.0.bin`** asset
(or `firmware.bin`). Publish tags as `v0.1.0`, `v0.1.1`, … — the device only offers an update when the
release tag is newer than the running version.

Upstream project and community support:
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
