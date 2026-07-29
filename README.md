# Casper

Personal firmware for **Xteink X3/X4**, based on
**[CrossPoint Reader 1.5.0](https://github.com/crosspoint-reader/crosspoint-reader/tree/release/1.5.0)**.

Casper keeps CrossPoint’s stable reader core and adds branding plus a
reading-first UI: **Dashboard home**, redesigned **reader chrome**, **reading
stats**, and a **StarDict dictionary** with multi-word selection and bilingual
packs.

## Themes

Home skins for different reading styles.

### Bare · Stats

<p align="center">
  <img src="./docs/images/casper/bare-theme.jpg" height="420" alt="Casper Bare theme home" />
  &nbsp;&nbsp;
  <img src="./docs/images/casper/dashboard.jpg" height="420" alt="Casper Stats theme home" />
</p>

<p align="center">
  <em><strong>Bare</strong> — just you and the book: cover, title, and simple actions.<br/>
  <strong>Stats</strong> — for power users and nerds who cannot get enough numbers.</em>
</p>

### Synopsis view

<p align="center">
  <img src="./docs/images/casper/synopsis-view.jpg" height="420" alt="Casper book synopsis view" />
</p>

<p align="center">
  <em><strong>Synopsis</strong> — open a short description of the current book without leaving home.</em>
</p>

## Highlights

| | |
|---|---|
| **Bare home** | Reading-first: cover-first layout, minimal chrome, Menu / Library / Synopsis / Read |
| **Stats home** | Cover-fill art, book metrics, lifetime stats, streak, Menu / Library / Settings / Read |
| **Reader chrome** | Battery, clock, % complete, time left, chapter, `Pg. n/m`, thin progress bar |
| **Dictionary** | StarDict folders, multi-pack cascade, multi-word selection, Spanish clitics/stems |
| **Stats tracking** | Per-book + lifetime; full Reading Stats screen; cache clear preserves progress/stats |

### More screenshots

<p align="center">
  <img src="./docs/images/casper/reader-ui.jpg" height="280" alt="Reader view" />
  &nbsp;
  <img src="./docs/images/casper/multi-word-selection.jpg" height="280" alt="Multi-word dictionary" />
  &nbsp;
  <img src="./docs/images/casper/english-definition.jpg" height="280" alt="English dictionary card" />
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
`0x10000`). Version label: **v0.1**.

## Credits

- [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — firmware base  
- Casper branding / UI overlay — this project  

## License

See [LICENSE](./LICENSE) (upstream MIT / project terms).
