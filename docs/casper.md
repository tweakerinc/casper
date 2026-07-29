---
title: Casper tour
nav_order: 2
description: Visual tour of Casper themes, reader, stats, controls, and dictionary
---

# Casper tour

**Casper** is personal firmware for the **Xteink X3/X4**, built on
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
It keeps a solid EPUB/TXT reading core, then layers Casper branding and a
reading-first UI: **Bare**, **Stats**, and **Stats-Life** home themes,
redesigned reader chrome, reading stats, flexible controls, and offline
dictionary lookup with multi-word selection.

[View on GitHub](https://github.com/tweakerinc/casper) ·
[Releases](https://github.com/tweakerinc/casper/releases) ·
[Installation](./installation.md)

---

<h2 align="center">Themes</h2>

<p align="center">
  Casper ships three home skins. Choose under <strong>Settings → Display → Theme</strong>.
</p>

| Bare | Stats | Stats-Life |
|:----:|:-----:|:----------:|
| <img src="./images/casper/bare-theme.jpg" alt="Casper Bare theme home" width="240" /> | <img src="./images/casper/stats-theme.jpg" alt="Casper Stats theme home" width="240" /> | <img src="./images/casper/stats-life-theme.jpg" alt="Casper Stats-Life theme home" width="240" /> |

### Bare

Designed for readers who simply want it to feel like **them and their book** —
a large cover, title and author, minimal chrome, and front-button actions
**Menu · Library · Synopsis · Read**.

- Long-press **Menu** → Settings  
- Long-press **Library** → Recents  
- Long-press **Read** → book quick menu  

### Stats

Built for people who want **detailed reading data** on the home screen:
reading time, time left, progress, daily average, pages per minute, book start
date, and estimated finish date. Front actions:
**Menu · Library · Settings · Read** (long-press **Read** → book menu).

### Stats-Life

Everything in **Stats**, plus **lifetime device stats** on the home screen under
the cover and book metrics.

> **Note:** Certain date/time features only work on the **X3** (the X4 has no RTC).

### Synopsis view

From Bare (or the book quick menu), open a short description of the current
book without diving into the file browser. Needs synopsis metadata (easy to add
in Calibre).

<img src="./images/casper/synopsis-view.jpg" alt="Casper book synopsis view" width="360" />

*Synopsis — title and blurb for the focused book.*

---

## Reading view

Chrome stays small so the page stays the focus: battery, clock, progress,
time left, chapter, and page number. Six independent slots (upper / lower ×
left / middle / right) are editable under **Customize Reader UI**.

<img src="./images/casper/reader-ui.jpg" alt="Casper reading view with status bar chrome" width="360" />

*Reader — top battery / clock / percent; bottom time left, chapter, Pg. n/m.*

---

## Reading stats

Full **Reading Stats** for the current book, plus device lifetime charts.

<img src="./images/casper/reading-stats.jpg" alt="Per-book reading stats with charts" width="360" />

*Per-book stats — sessions, time, progress, pace, start / est. finish, habit charts.*

<img src="./images/casper/lifetime-stats.jpg" alt="Device lifetime reading stats with charts" width="360" />

*This device — lifetime totals plus time-of-day and day-of-week breakdowns.*

Tracking can be turned off under **Settings → System → Stats** for a
distraction-free setup. Auto backup and **Backup Now** live there too.

---

## Controls

Defaults tuned for everyday use: short power sleeps, long power refreshes,
long-press menu opens the dictionary. Buttons are fully remappable.

<img src="./images/casper/controls.jpg" alt="Settings Controls tab" width="360" />

*Settings → Controls — Short-Press Power = Sleep, Long-Press Menu = Dictionary.*

---

## Settings overview

Tabbed **Settings** with Display, Reader, Controls, and System categories.

<img src="./images/casper/settings.jpg" alt="Casper Settings screen" width="360" />

*Settings — Display tab with Theme, Status Bar, Sleep Screen, and more.*

---

## Customize Reader UI

Slot-based reader chrome with live preview: battery modes, progress bar,
page counters, titles, and time left — independent of the system status bar.

<img src="./images/casper/customize-reader-ui.jpg" alt="Customize Reader UI six-slot editor" width="360" />

*Customize Reader UI — six exclusive slots and nested battery display modes.*

---

## Manage Fonts

Tabs for **Font · Size · Layout · Style**, roughly 50/50 list vs live preview.
Default built-ins: **Source Serif 4** and **Bitter**. Download Fonts is a row
in the Font list.

<img src="./images/casper/manage-fonts.jpg" alt="Manage Fonts text settings" width="360" />

*Manage Fonts — live preview of family, size, and layout.*

---

## Dictionary

Offline packs on the SD card. Multi-select which dictionaries cascade on lookup.
Install by extracting a `.dictionaries` folder to the root of the SD card.

### Pack selection

<img src="./images/casper/dictionary-settings.jpg" alt="Dictionary pack multi-select screen" width="360" />

*Dictionary settings — enable English, English–Spanish, Spanish–English packs.*

### Definition card

One lookup can show English senses and a bilingual gloss in the same card,
with pronunciation beside the headword. Long entries scroll inside a card that
stays clear of the status bar and menu.

<img src="./images/casper/dictionary-lookup.jpg" alt="Dictionary definition card" width="360" />

*Dictionary lookup — headword, pronunciation, and multi-pack senses.*

### Spanish clitics

Forms like **ayúdame** resolve to the verb stem with a natural “help me” line.

<img src="./images/casper/spanish-english.jpg" alt="Dictionary card for ayudar from Spanish clitic" width="360" />

*`Ayúdame` → ayudar / help me (Spanish–English).*

### Multi-word selection

Long-press **Select** to arm a range, move with Left/Right, short **Select** to
look up the phrase (e.g. collocations like `por favor`).

<img src="./images/casper/multi-word-selection.jpg" alt="Multi-word selection of por favor" width="360" />

*Multi-word selection — `por favor` → “please”.*

More install detail: [Dictionary](./dictionary.md).

---

## Highlights at a glance

- **Casper** branding (boot, portal, serial / **v0.1.x**)
- **Bare · Stats · Stats-Life** home themes
- Quiet reader chrome + customizable system / reader status bars
- **Reading stats** per book and this device (optional tracking off)
- **Controls** defaults: sleep / refresh / dictionary + remapping
- **Offline dictionary** with multi-pack cascade and multi-word selection

---

## Get the firmware

1. Open **[Releases](https://github.com/tweakerinc/casper/releases)**  
2. Download the `.bin` for your build  
3. Follow **[Installation](./installation.md)**  

**On-device OTA:** after the first install, use **Settings → System → Check for updates**.
That hits this repo’s latest GitHub Release. Publish tags as `v0.1.0`, `v0.1.1`, …
— the device only offers an update when the release tag is newer than the
running version.

Upstream project and community support:
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
