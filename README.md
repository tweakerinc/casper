> Project Name: **Casper**
> 
> **This is a personal firmware project built on [Crosspoint Reader 1.5](https://github.com/crosspoint-reader/crosspoint-reader) & [CrossInk](https://github.com/uxjulia/crossink)** for the Xteink X3.  


## What's different in Casper?

Casper is a personal build, rebranded and tuned for how I actually use the Xteink X3 day to day. This will flash to both the X3 and X4, however the dashboard is a little different for the X4 as it lacks a Real Time Clock for the date tracking.

My goal was to maximize the usable space on the dashboard as well as implement a dictionary. I was able to fit lifetime stats at the bottom with some rearranging. I personally enjoy looking at them, but did not like having to go into the menu to get to them. I made many minor changes to spelling, punctuation, and verbiage, in various menus. I also added the ability for you to select what appears in the four corners of your screen when you are reading a book for the ultimate personalization. You could disable everything for a minimalist experience, or you could choose between Hide, Battery, Page Counter, Progress Percentage, Time left in Book, or Time left in Chapter for each corner. There are a dozen or more other tiny changes like those throughout. The other notable additions are an offline dictionary, and an auto upload feature for KoReader Sync. I am really happy with how the dictionary looks and produces information, but I am always open to suggestions and improvements.


---

Below Images are all from an XTEINK X3. This firmware will work with the X4 but some of the features will be gated, simply becaue the X4 does not have an RTC (Real Time Clock). My main focus thus far has been the X3 but I will be spending a bit more time on the X4.

## Themes

<table>
  <tr>
    <td align="center" valign="top" width="50%">
      <h3>Bare</h3>
      <img src="./docs/images/casper/bare-theme.jpg?v=20260728" alt="Casper Bare theme home" width="320" /><br/>
      <em>Just you and the book: cover, title, and simple actions focused on reading.</em>
    </td>
    <td align="center" valign="top" width="50%">
      <h3>Stats</h3>
      <img src="./docs/images/casper/dashboard.jpg?v=20260728" alt="Casper Stats theme home" width="320" /><br/>
      <em>For power users and nerds who cannot get enough numbers: metrics, lifetime totals, streak.</em>
    </td>
  </tr>
</table>

<!-- valign=top so short captions (e.g. "Reader UI") don't sink lower than longer ones on mobile -->
<table>
  <tr>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/synopsis-view.jpg?v=20260728" alt="Book synopsis" width="280" /><br/>
      <em>Synopsis</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/reader-ui.jpg" alt="Reading view" width="280" /><br/>
      <em>Reader UI</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/reading-stats.jpg" alt="Reading stats" width="280" /><br/>
      <em>Reading Stats Per Book</em>
    </td>
  </tr>
  <tr>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/lifetime-stats.jpg" alt="Device stats" width="280" /><br/>
      <em>Device Lifetime Stats</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/english-definition.jpg" alt="English dictionary" width="280" /><br/>
      <em>Dictionary Lookup</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/multi-word-selection.jpg" alt="Multi-word selection" width="280" /><br/>
      <em>Multi-Word Lookup</em>
    </td>
  </tr>
  <tr>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/spanish-english.jpg" alt="Spanish-English" width="280" /><br/>
      <em>Spanish Translation</em>
    </td>
    <td></td>
    <td></td>
  </tr>
</table>

**Full photo tour with captions:** **[docs/casper.md](./docs/casper.md)**

---

**Note**: Target hardware is the **Xteink X3 & X4** (ESP32-C3). Flash at your own risk; keep a stock CrossPoint `.bin` so you can revert.

### Features

#### Dashboard
Improved layout based on [CrossInk](https://github.com/uxjulia/crossink)'s dashboard (the best starting point). Space is used more efficiently so **Lifetime Stats** fit cleanly on the main home screen alongside the current book, progress, and glanceable book stats.

- **X3** (RTC): day streak, reader type, daily average, started / finish dates
- **X4** (no RTC): session and pace metrics in place of calendar-only fields
- One firmware image for **both** devices; only hardware-backed stats are gated

Empty library still shows the full dashboard shell (frames and labels) so the home screen never looks broken before you open a first book.

#### Offline Dictionary
Popup definitions with pronunciation and numbered senses — formatting tuned for e-ink.

- **Long-press Menu** while reading → dictionary word selection on the page
- **Long-press Select** in the dictionary tool → multi-word selection (extend with Left/Right, short Select to look up)
- Soft hyphens and end-of-line splits are joined so compounds still match
- Multiple **StarDict** packs at once (e.g. English + Spanish–English), with stems and collocation fallbacks

**Install:** put dictionary packs under `/dictionaries/` (or `/.dictionaries/`) on the SD card, then **Settings → Reader → Dictionary** and enable the packs you want. English and Spanish packs are the ones I ship and use; see [Dictionary](./docs/dictionary.md).

#### Customizable Reader UI
Choose what appears in every corner of the reading screen (or hide everything for a clean page).

My preferred layout:

| | Left | Center | Right |
|--|------|--------|-------|
| **Top** | Battery | Clock | % complete |
| **Bottom** | Time left (chapter or book) | Chapter title | Chapter pages (`Pg. n/m`) |

Any element can be shown, hidden, or moved under **Settings → Reader → Customise Status Bar**. Thin book progress bar by default.

#### Menu & UI improvements
- Clearer spelling and wording throughout menus
- Settings kept reachable with fewer clicks; **side buttons scroll horizontal Settings tabs** instead of only walking the full list
- **Long-press Read** on the Dashboard → quick book menu (stats, reset pace, mark finished, remove from recent, etc.)
- Front home map on Dashboard: **Menu · Browse · Settings · Read**
- Improved **Manage Fonts** flow with a more accurate, full-width preview
- Default reading face: **Lexend Deca 12**
- Double-line Settings header / tab chrome for clearer hierarchy

**Controls (defaults)**

| Action | Behavior |
|--------|----------|
| Short-press Power | Sleep / wake |
| Long-press Power | Screen refresh |
| Long-press Menu | Dictionary (while reading) |
| Long-press Select (in dictionary) | Multi-word selection |
| Side long-press | Off (no multi-page skip when resting a finger) |

#### KOReader Sync
Sign up / authenticate (default `sync.crosspointreader.com`). **Sync Behavior** is four exclusive modes (pick one):

| Mode | Kind | What it does |
|------|------|----------------|
| **Ask Every Time** | Manual | Sync Progress asks **Apply Remote** vs **Upload Local**. No auto-upload on leave. |
| **Smart Sync** | Manual | CrossPoint 1.5-style: furthest progress / already synced. No auto-upload on leave. |
| **Percent** | Auto-upload | On leave, upload if progress advanced enough (default **1%**). |
| **Time** | Auto-upload | On leave, upload if enough time passed (default **1 hour**). |

Example: automatically upload when closing a book if you've read more than 1%, or if more than an hour has passed since the last upload.

Full setup: [docs/koreader-sync.md](./docs/koreader-sync.md).

#### Also included
- **Casper branding** — boot logo, web portal titles, serial / version strings
- **Reading Stats** screens for the current book and this device (open from the long-press book menu)
- Works on **Xteink X3 and X4** from one build

For version-by-version notes, see [Releases](https://github.com/TweakerInc/casper/releases).
---

### Dictionary

Casper looks up words offline from **StarDict** folders on the SD card under `/dictionaries/` (or `/.dictionaries/`). One folder per pack (e.g. `English/`, `Spanish-English/`).

See [Dictionary](./docs/dictionary.md) for pack layout and install notes.

### KOReader Sync setup

1. **Settings → System → KOReader Sync**
2. Enter **Username** / **Password** (same on all devices).
3. **Sign Up** (first device) or **Authenticate** (others).
4. Leave **Sync Server URL** empty for `sync.crosspointreader.com`, or set `https://sync.koreader.rocks`.
5. **Sync Behavior** (popup — pick **one**):
   - **Ask Every Time** / **Smart Sync** — manual Sync Progress only (Smart Sync = CrossPoint 1.5 auto-resolve)
   - **Percent** / **Time** — auto-upload when leaving a book (defaults 1% / 1 hour); not combined with Smart Sync

Full guide: **[docs/koreader-sync.md](./docs/koreader-sync.md)**.

### Reader features & controls

- [Reader Features](./docs/reader-features.md) — options, stats, and related notes  
- [Controls](./docs/controls.md) — shortcuts and remapping  
- [KOReader Sync](./docs/koreader-sync.md) — Sign Up, servers, Smart Sync, auto-upload  




---

## Tips for the best reading experience

Casper runs on an ESP32-C3 with limited RAM (~380 KB usable). Large folders or complex EPUBs can be slower than on a phone or tablet.

- Keep folders under about 200 files (50–100 is smoother).
- Split a large library into folders (author, series, read/unread).
- Prefer text-first EPUBs; image-heavy / comic / huge omnibus files are memory-sensitive.
- Rough target: EPUBs under ~20 MB behave best; over ~50 MB may still work but can be slow.
- Use File Transfer’s EPUB optimizer when a book is painful: [Web server](./docs/webserver.md).
- Use a reliable SD card and leave free space for cache, progress, and stats.

---

## Installation

1. Download a `firmware-*.bin` from the [releases page](https://github.com/TweakerInc/casper/releases).
2. Flash with the CrossPoint web installer (**Custom .bin**) or `esptool`.

Step-by-step: **[Installation](./docs/installation.md)**  
To go back to official firmware, reflash from [crosspointreader.com](https://crosspointreader.com/#flash-tools).

---

## Documentation

- [**Casper Tour (photos)**](./docs/casper.md)
- [User Guide](./USER_GUIDE.md)
- [Installation](./docs/installation.md)
- [Dictionary](./docs/dictionary.md)
- [KOReader Sync](./docs/koreader-sync.md)
- [Font Build Variants](./docs/font-build-variants.md)
- [Reader Features](./docs/reader-features.md)
- [Controls](./docs/controls.md)
- [Simulator](./docs/simulator.md)
- [Data Cache](./docs/data-cache.md)
- [Web server](./docs/webserver.md)
- [Troubleshooting](./docs/troubleshooting.md)
- [Project scope](./SCOPE.md)
- [Publishing this project](./docs/PUBLISHING.md)

Docs site (after GitHub Pages is enabled): same files under `docs/` via Just the Docs.

---

## Development quick start

PlatformIO:

```sh
pio run -e default
# or tiny / xlarge — see font-build-variants.md
pio run -e default --target upload
```

See [Getting Started](./docs/contributing/getting-started.md) if present, and [AGENTS.md](./AGENTS.md) for embedded constraints.

---

## Internals

Reusable book/device state lives under `.crosspoint` on the SD card.

- [Data Cache](./docs/data-cache.md)
- [File Formats](./docs/file-formats.md)

---

## Credits & license

- Upstream: [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) and the broader community.
- Casper is an independent personal build; it is **not** an official CrossPoint or CrossInk release.
- See [LICENSE](./LICENSE) (MIT, same family as upstream unless noted otherwise).
