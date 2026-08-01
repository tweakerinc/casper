# Casper

Personal firmware for **Xteink X3/X4**, based on
**[CrossPoint Reader 1.5.0](https://github.com/crosspoint-reader/crosspoint-reader/tree/release/1.5.0)**.

Casper keeps CrossPoint’s stable reader core and adds branding plus a
reading-first UI: **Penumbra** (X3 clock home / X4 title home) and **Bare**,
redesigned **reader chrome**, **reading stats**, and a **StarDict dictionary**
with multi-word selection and bilingual packs.

<h2 align="center">Themes</h2>

<p align="center">
  Home skins for different reading styles. Pick one under <strong>Settings → Display → Theme</strong>.
</p>

| Penumbra (X3) | Penumbra (X4) | Bare |
|:-------------:|:-------------:|:----:|
| <img src="./docs/images/casper/x3-penumbra-home.jpg" alt="Penumbra home on X3" width="260" /> | <img src="./docs/images/casper/x4-penumbra-home.jpg" alt="Penumbra home on X4" width="260" /> | <img src="./docs/images/casper/bare-home.jpg" alt="Bare Theme" width="260" /> |

> **Note:** Clock, weekday, and certain date/time features on Penumbra need the **X3** RTC (the X4 has no real-time clock). Bare looks the same on both devices.

<h2 align="center">Gallery</h2>

| Penumbra Recents | Penumbra Book Stats | Penumbra Lifetime | Synopsis |
|:----------------:|:-------------------:|:-----------------:|:--------:|
| <img src="./docs/images/casper/x3-penumbra-recents.jpg" alt="Penumbra Recents" width="200" /> | <img src="./docs/images/casper/x3-penumbra-book-stats.jpg" alt="Penumbra Book Stats" width="200" /> | <img src="./docs/images/casper/x3-penumbra-life-stats.jpg" alt="Penumbra Lifetime Stats" width="200" /> | <img src="./docs/images/casper/synopsis-view.jpg" alt="Synopsis" width="200" /> |

| Reading UI | Multi-Word Lookup | Dictionary Lookup | Reading Stats |
|:----------:|:-----------------:|:-----------------:|:-------------:|
| <img src="./docs/images/casper/reader-ui.jpg" alt="Reading UI" width="200" /> | <img src="./docs/images/casper/multi-word-selection.jpg" alt="Multi-Word Lookup" width="200" /> | <img src="./docs/images/casper/dictionary-lookup.jpg" alt="Dictionary Lookup" width="200" /> | <img src="./docs/images/casper/reading-stats.jpg" alt="Reading Stats" width="200" /> |

| Lifetime Stats | Controls | Dictionary Settings | Spanish Translation |
|:--------------:|:--------:|:-------------------:|:-------------------:|
| <img src="./docs/images/casper/lifetime-stats.jpg" alt="Lifetime Reading Stats" width="200" /> | <img src="./docs/images/casper/controls.jpg" alt="Controls" width="200" /> | <img src="./docs/images/casper/dictionary-settings.jpg" alt="Dictionary Settings" width="200" /> | <img src="./docs/images/casper/spanish-english.jpg" alt="Spanish Translation" width="200" /> |

| Manage Fonts | Customize Reader UI | Settings |
|:------------:|:-------------------:|:--------:|
| <img src="./docs/images/casper/manage-fonts.jpg" alt="Manage Fonts" width="200" /> | <img src="./docs/images/casper/customize-reader-ui.jpg" alt="Customize Reader UI" width="200" /> | <img src="./docs/images/casper/settings.jpg" alt="Settings" width="200" /> |

**Full photo tour:** **[docs/casper.md](./docs/casper.md)**

---

## Features

Feature overview from **[Casper v0.1.3](https://github.com/tweakerinc/casper/releases/tag/v0.1.3)** (New Themes and UI Overhaul). Full release notes and firmware assets are on the [Releases](https://github.com/tweakerinc/casper/releases) page.

### Themes

#### Penumbra

Text-first home with under-panels you cycle on the side buttons.

- **X3:** large clock + weekday, then **Title · Recents · Book Stats · Lifetime** under the hairline (side Left/Right).  
- **X4:** last-read title/author on top, **Recents** list below (side Up/Down scrolls the list).  

`Menu` · `Library` · `Recents` · `Read`  
(+ long-press **Menu** → Settings · long-press **Read** → book quick menu)

#### Bare

Designed for readers who simply want to feel as if it is them and their book — a minimal cover theme, same layout on X3 and X4.

`Menu` · `Library` · `Recents` · `Read`

- Long-press **Menu** → Settings  
- Long-press **Read** → book quick menu  

> **Note:** Clock and weekday on Penumbra need the **X3** RTC (the X4 has no real-time clock).

### System + Reader Status Bars

Fully customizable status bars with slot-based placement and live previews.  
Choose exactly what appears in the top status bar and position items in any of the **6 available slots** (Top/Bottom × Left/Middle/Right) independently for the **system UI** and the **reader**.

### Battery Display Options

Independent control for system and reader:

- Hidden  
- Icon only  
- Percent only  
- Icon + Percent  

### Manage Fonts

Completely redesigned with tabs, 50/50 live preview, a Download Fonts row, and improved organization.  
Default fonts are now **Source Serif 4** and **Bitter**.  
This is where you set font, size, layout, and style for how books are displayed, with a clean preview of every change.

### Tabbed Navigation + Button Remapping

Fully customizable button remapping.  
Front buttons act as a list; side buttons can be set as tabs (or whatever you prefer).  
A popular setup is using the side buttons as left/right controls to scroll through horizontal tabs.

### System → Stats Folder

New options under **Settings → System → Stats**:

- Enable / Disable Stat Tracking  
- Auto Backup  
- Backup Now  

You can turn stats off completely if you prefer a distraction-free reading experience.

### Cover Thumbnail Pipeline

Improved cover generation for clearer, higher-quality thumbnails.

### UI Polish

Unified design language across the interface:

- Black chips instead of grey highlights  
- Better title centering  
- Cleaner version string  
- Fixed long-press behavior in the Library  

### Book Synopsis

Open a short description of the current book from the **book quick menu** (long-press **Read** on home, or long-press a book in the library). Requires synopsis metadata (easy to add with Calibre).

### Reader Shortcuts

- Long-press **Select** → Dictionary  
- Long-press **Select** again while in dictionary → Multi-word select mode  
- **Menu** button → Reader options menu  

### Dictionary

Offline StarDict packs on the SD card, with multi-pack cascade and multi-word selection.

To install dictionaries, extract the `.dictionaries` folder to the **root of your SD card**.

---

## Docs

| Doc | Contents |
|-----|----------|
| [docs/casper.md](./docs/casper.md) | Visual showcase of Casper changes (photos) |
| [CASPER.md](./CASPER.md) | Scope, APIs, defaults, implementation notes |
| [docs/dictionary.md](./docs/dictionary.md) | StarDict install and lookup behavior |
| [dist/dictionaries/README.txt](./dist/dictionaries/README.txt) | Shipping dictionary packs |

## Build

```bat
pio run -e default
```

Flash with the CrossPoint web installer (**Custom .bin**) or `esptool` (app at
`0x10000`). Version label: **v0.1.4** (latest tag: **v0.1.3**).

## Credits

- [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — firmware base  
- Casper branding / UI overlay — this project  

## License

See [LICENSE](./LICENSE) (upstream MIT / project terms).
