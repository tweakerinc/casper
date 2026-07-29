# Casper

Personal firmware for **Xteink X3/X4**, based on
**[CrossPoint Reader 1.5.0](https://github.com/crosspoint-reader/crosspoint-reader/tree/release/1.5.0)**.

Casper keeps CrossPoint’s stable reader core and adds branding plus a
reading-first UI: **Bare**, **Stats**, and **Stats-Life** home themes,
redesigned **reader chrome**, **reading stats**, and a **StarDict dictionary**
with multi-word selection and bilingual packs.

<h2 align="center">Themes</h2>

<p align="center">
  Home skins for different reading styles. Pick one under <strong>Settings → Display → Theme</strong>.
</p>

| Bare | Stats | Stats-Life |
|:----:|:-----:|:----------:|
| <img src="./docs/images/casper/bare-home.jpg" alt="Casper Bare theme home" width="260" /> | <img src="./docs/images/casper/stats-theme.jpg" alt="Casper Stats theme home" width="260" /> | <img src="./docs/images/casper/stats-life-theme.jpg" alt="Casper Stats-Life theme home" width="260" /> |

| Theme | Who it’s for | Home actions |
|-------|----------------|--------------|
| **Bare** | Readers who want it to feel like just them and the book — large cover, title/author, minimal chrome | **Menu · Library · Synopsis · Read** (long-press Menu → Settings, Library → Recents, Read → book menu) |
| **Stats** | Power users who want detailed book data: reading time, time left, progress, daily average, pages/min, start & est. finish | **Menu · Library · Settings · Read** |
| **Stats-Life** | Everything in **Stats**, plus **lifetime device stats** on the home screen | Same as Stats |

> **Note:** Certain date/time features only work on the **X3** (the X4 has no real-time clock).

### Synopsis

<p align="center">
  <img src="./docs/images/casper/synopsis-view.jpg" height="360" alt="Casper book synopsis view" />
</p>

<p align="center">
  <em><strong>Synopsis</strong> — short blurb for the current book (Bare home button or long-press book menu).</em>
</p>

## Highlights

| | |
|---|---|
| **Bare home** | Cover-first, minimal chrome, Synopsis on the front row |
| **Stats / Stats-Life** | Cover + book metrics; Stats-Life adds lifetime totals on home |
| **Reader chrome** | Six-slot status bars, battery modes, progress, time left, chapter, pages |
| **Dictionary** | StarDict packs, multi-pack cascade, multi-word selection, Spanish clitics |
| **Stats tracking** | Per-book + lifetime; optional tracking off; backup under System → Stats |

### More screenshots

<p align="center">
  <img src="./docs/images/casper/reader-ui.jpg" height="260" alt="Reader view" />
  &nbsp;
  <img src="./docs/images/casper/multi-word-selection.jpg" height="260" alt="Multi-word dictionary" />
  &nbsp;
  <img src="./docs/images/casper/dictionary-lookup.jpg" height="260" alt="Dictionary definition card" />
</p>
<p align="center">
  <img src="./docs/images/casper/reading-stats.jpg" height="260" alt="Per-book reading stats" />
  &nbsp;
  <img src="./docs/images/casper/lifetime-stats.jpg" height="260" alt="Lifetime device stats" />
  &nbsp;
  <img src="./docs/images/casper/controls.jpg" height="260" alt="Controls settings" />
</p>
<p align="center">
  <img src="./docs/images/casper/dictionary-settings.jpg" height="260" alt="Dictionary pack selection" />
  &nbsp;
  <img src="./docs/images/casper/spanish-english.jpg" height="260" alt="Spanish–English lookup" />
  &nbsp;
  <img src="./docs/images/casper/manage-fonts.jpg" height="260" alt="Manage Fonts" />
</p>
<p align="center">
  <img src="./docs/images/casper/customize-reader-ui.jpg" height="260" alt="Customize Reader UI" />
  &nbsp;
  <img src="./docs/images/casper/settings.jpg" height="260" alt="Settings" />
</p>

**Full photo tour:** **[docs/casper.md](./docs/casper.md)**

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
