---
title: Dictionary
nav_order: 5
---

# Dictionary

Casper looks up words while reading using **offline StarDict packs** on the SD card.

Casper does **not** use the older CXDict single-file format (`en.cxdict` under `/.crosspoint/dict/`). Everything is **one folder per dictionary** under `/dictionaries/` (or `/.dictionaries/`).

---

## SD card layout

### Roots (both supported)

| Root | Notes |
|------|--------|
| `/dictionaries/` | Preferred. Visible in Browse when hidden files are shown. |
| `/.dictionaries/` | Same rules; hidden by default in the file browser. |

Discovery scans both roots. Folder names must not start with `.` and must not contain `/` or `\`.

### One folder per pack

```text
/dictionaries/
  English/
    english.ifo          # optional metadata
    english.idx          # required, uncompressed
    english.dict         # required (or english.dict.dz)
    english.qidx         # optional; device-generated index sidecar
  English-Spanish/
    …
  Spanish-English/
    …
```

| Rule | Detail |
|------|--------|
| Folder name | Label in **Settings → Reader → Dictionary** (e.g. `English`). |
| Single stem | Exactly **one** `.idx` basename inside the folder. |
| Data file | Same stem with `.dict` **or** `.dict.dz` must exist. |
| Uncompressed index | `.idx.gz` is **not** supported; decompress before copy. |
| Optional `.ifo` | Recommended. Rejects `idxoffsetbits=64`. |
| Ignored | `.syn` synonym files. |

### `.qidx` (device-built)

Not part of a release zip. On first lookup (or when the index changes), the firmware writes a small sampled-offset table next to the `.idx`. Safe to delete; it rebuilds automatically (UI may show “Indexing…” once per pack).

---

## Bundled packs

Ship or copy whole folders under `/dictionaries/`:

| Folder | Role |
|--------|------|
| `English/` | English definitions |
| `English-Spanish/` | English → Spanish |
| `Spanish-English/` | Spanish → English |

Enable one or more packs under **Settings → Reader → Dictionary**. Multi-select cascades results with pack headers on the definition card. On first dictionary open with nothing selected, firmware may auto-enable every installed pack.

---

## Use it while reading

1. Open an EPUB.
2. Long-press **Menu** (default) to open **Dictionary Lookup**, or choose Dictionary from the reader menu if available.
3. Move to a word with Left/Right (and Up/Down by line). Soft hyphens and end-of-line hyphen splits are joined for lookup.
4. Short **Select** looks up the word.
5. **Multi-word:** long-press **Select** to arm a range, move to extend, short **Select** to look up the phrase.
6. Definition card overlays the page (headword, optional pronunciation, pack sections). **Back** / **Done** dismisses.

### Lookup behavior

- Punctuation and quotes are stripped; Spanish accents kept where possible (with an accent-fold fallback).
- English-style stems (plurals, `-ing`, etc.).
- Spanish object clitics (`ayúdame` → `ayuda` / `ayudar`) with a short natural phrase when the pack is bilingual.
- Multi-word selections try the full phrase, collocation windows (e.g. `por favor`), then each token with the same rules.

---

## Notes

- Stock CrossPoint **StarDict** reader — not CXDict and not a full desktop StarDict feature set.
- Arbitrary idioms only match if they exist as headwords or can be recovered via windows/stems.
- Clear or reinstall packs on the SD card if you rename folders; settings store folder names.

More UI detail: [Casper tour](./casper.md#dictionary).
