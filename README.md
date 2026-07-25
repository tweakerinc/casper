> **This is a personal firmware project built on [Crosspoint Reader 1.5](https://github.com/crosspoint-reader/crosspoint-reader) & [CrossInk](https://github.com/uxjulia/crossink)** for the Xteink X3.  
> Product name: **Casper**).

## What's different in Casper?

Casper is a personal build, rebranded and tuned for how I actually use the Xteink X3 day to day. My goal was to maximize the usable space on the dashboard. I was able to fit lifetime stats at the bottom by rearranging things a bit. I also changed the settings so you can now choose what you put in each of the four Corners of your Reader UI. You can choose between Battery, Page Counter, Progress Percentage, Timee Left in Book, and Time Left in Chapter. You can also choose to hide any corner if you prefer a minimalist view. I made many minor changes to spelling, punctuation, verbiage, in various menus. There are a dozen or more other tiny changes like those.

<p align="center">
  <img src="./docs/images/casper/dashboard.jpg" alt="Casper Dashboard home" width="360" />
</p>

<p align="center"><em>Dashboard Home Screen — Current Book, Reading Stats for Book & Lifetime Device, Streak</em></p>

<!-- valign=top so short captions (e.g. "Reader UI") don't sink lower than longer ones on mobile -->
<table>
  <tr>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/reading-view.jpg" alt="Reading view" width="280" /><br/>
      <em>Reader UI</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/reading-stats.jpg" alt="Reading stats" width="280" /><br/>
      <em>Reading Stats Per Book</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/lifetime-stats.jpg" alt="Device stats" width="280" /><br/>
      <em>Device Lifetime Stats</em>
    </td>
  </tr>
  <tr>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/english-definition.jpg" alt="English dictionary" width="280" /><br/>
      <em>Dictionary Lookup</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/multi-word-selection.jpg" alt="Multi-word selection" width="280" /><br/>
      <em>Multi-Word Lookup</em>
    </td>
    <td align="center" valign="top" width="33%">
      <img src="./docs/images/casper/spanish-english.jpg" alt="Spanish-English" width="280" /><br/>
      <em>Spanish Translation</em>
    </td>
  </tr>
</table>

**Full photo tour with captions:** **[docs/casper.md](./docs/casper.md)**

---

**Note**: Target hardware is the **Xteink X3** (ESP32-C3). Flash at your own risk; keep a stock CrossPoint `.bin` so you can revert.

### Casper highlights

- **Casper Branding** — Custom boot logo of a cute ghost, web portal, serial version strings.
- **Dashboard** — Dashboard Home Screen is essentially the same as [CrossInk](https://github.com/uxjulia/crossink). Lifetime stats were added and the layout was rearranged to make the most of the space.
- **Offline Dictionary** — I spent a lot of time trying to get the look right. I wanted a pop up window with pronunciation, and numbered definitions. Long-press Menu opens word selection on the current page. Move to a word and press Select to look it up. Long-press Select while in the dictionary tool starts multi-word selection; extend with Left/Right, then short Select to look up the phrase. Soft-hyphen and end-of-line splits are joined so compounds still match. Multiple **StarDict** packs can run at once (e.g. English + Spanish–English), with stems, Spanish clitics, and collocation windows when the full phrase isn’t a headword.
- **Control defaults**
  - Short-Press Power → **Sleep**
  - Long-Press Power → **Force refresh**
  - Long-Press Menu → **Dictionary**
  - Side long-press → **Off** (no multi-page when resting a finger)
- **Reader UI** — Battery top-left; time remaining shows scope (`in Chapter` / `in Book`); percent complete top-right; pages bottom-right with a `Pg.` prefix.
- **KOReader Sync** — CrossPoint 1.5 setup (Sign Up / Authenticate, default `sync.crosspointreader.com`), plus Casper **auto-upload on close** (Time / Percent / Adaptive). **Sync Behavior**: **Ask Every Time** (choose Apply vs Upload) or **Smart Sync** (auto-resolve). See [docs/koreader-sync.md](./docs/koreader-sync.md).

For version-by-version notes, see [Releases](https://github.com/TweakerInc/casper/releases).

---

### Dictionary

Casper looks up words offline from **StarDict** folders on the SD card under `/dictionaries/` (or `/.dictionaries/`). One folder per pack (e.g. `English/`, `Spanish-English/`).

See [Dictionary](./docs/dictionary.md) for pack layout and install notes.

### KOReader Sync setup (1.5)

1. **Settings → System → KOReader Sync**
2. Enter **Username** / **Password** (same on all devices).
3. Leave **Sync Server URL** empty for the CrossPoint server (`sync.crosspointreader.com`), **or** set `https://sync.koreader.rocks` for the legacy public server.
4. On the **first** device: **Sign Up**. On others: **Authenticate** only.
5. **Sync Behavior** → **Ask Every Time** if you want to choose Apply/Upload each manual sync; **Smart Sync** auto-resolves.
6. Optional: **Auto Upload on Close** + **Upload Type** (Time / Percent / Adaptive).

Full steps (including self-hosted): **[docs/koreader-sync.md](./docs/koreader-sync.md)**.

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
