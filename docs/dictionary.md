# Dictionary

CrossPoint looks up words while reading using **offline StarDict packs** on the SD card. This document describes the **current on-device file structure**, how packs are discovered, and how lookup works in firmware.

CrossPoint does **not** use CXDict (the experimental single-binary format). Everything is **one folder per dictionary** under `/dictionaries/` (or `/.dictionaries/`).

---

## SD card layout

### Roots (both supported)

| Root | Notes |
|------|--------|
| `/dictionaries/` | Preferred. Visible in Browse when hidden files are shown. |
| `/.dictionaries/` | Same rules; hidden by default in the file browser. |

Discovery scans both roots. Folder names must not start with `.` and must not contain `/` or `\` (they are stored in settings JSON).

### One folder per pack

```
/dictionaries/
  English/
    english.ifo          # optional metadata
    english.idx          # required, uncompressed
    english.dict         # required (or english.dict.dz)
    english.qidx         # optional; device-generated index sidecar
  English-Spanish/
    english-spanish.ifo
    english-spanish.idx
    english-spanish.dict
  Spanish-English/
    spanish-english.ifo
    spanish-english.idx
    spanish-english.dict
```

| Rule | Detail |
|------|--------|
| Folder name | Label in **Settings → Reader → Dictionary** (e.g. `English`). |
| Single stem | Exactly **one** `.idx` basename inside the folder. Multiple stems → folder skipped. |
| Data file | Same stem with `.dict` **or** `.dict.dz` must exist. |
| Uncompressed index | `.idx.gz` is **not** supported; decompress with `gzip -d` before copy. |
| Ambiguous / empty | No `.idx`, only `.idx`, or multiple stems → not listed. |
| Optional `.ifo` | Recommended. Used for validation (e.g. reject `idxoffsetbits=64`). |
| Ignored | `.syn` synonym files. |

### What each file is

#### `.ifo` (optional text)

StarDict info file. CrossPoint release packs use:

```text
version=2.4.2
wordcount=…
idxfilesize=…
bookname=…
sametypesequence=m
```

- `sametypesequence=m` → plain UTF-8 definition text (what the UI expects).
- **32-bit** word offsets only. `idxoffsetbits=64` is rejected.

#### `.idx` (required)

Sorted headword index. Each entry:

```text
UTF-8 headword + 0x00 + uint32_be(offset) + uint32_be(size)
```

- Offset/size refer to bytes inside `.dict` (or uncompressed stream of `.dict.dz`).
- Matching is **case-insensitive** (ASCII fold).

#### `.dict` / `.dict.dz` (required)

- `.dict` — raw concatenation of definition blobs (fastest path).
- `.dict.dz` — dictzip (random-access gzip); supported, slightly slower first read.

Definitions in the shipped packs are plain text (`m`), often multi-line (POS, gender, senses, optional pronunciation).

#### `.qidx` (device-built)

Not part of the release zip. On first lookup (or when the `.idx` changes / sample interval changes), the firmware writes a small **sampled offset table** next to the `.idx`:

- Magic / version / sample interval / sample count / `.idx` size.
- One `uint32` file offset per sample (every **64** index entries in current firmware).

Safe to delete; it is rebuilt automatically (UI may show “Indexing…” once per pack).

---

## Bundled release packs

The GitHub / dist zip ships three folders (copy as-is under `/dictionaries/`):

| Folder | Role |
|--------|------|
| **English** | English headword → English definition |
| **English-Spanish** | English headword → Spanish |
| **Spanish-English** | Spanish headword → English |

Why all three:

- EN-ES alone only helps when the **selected page word is English**.
- Spanish tokens (`casa`, `porque`, `ayúdame`) need **Spanish-English**.
- Firmware also expands **Spanish clitics** (`ayudame` → `ayuda` / `ayudar`).

**Recommendation:** enable all three in multi-select.

Sources (this generation of packs):

- EN: Wiktionary (kaikki.org) + Open English WordNet + public-domain Webster gaps  
- EN-ES / ES-EN: Wiktionary via open-dsl-dict (CC BY-SA / GFDL)  

Developer rebuild: `scripts/build_stardict_packs.py`.

---

## Enabling packs on the device

1. Copy folders to `/dictionaries/` (or `/.dictionaries/`).
2. **Settings → Reader → Dictionary** — multi-select list with `(*)` / `( )`.
3. After a change, **Back** becomes **Save**.

If no pack is selected yet, the **first** dictionary open from the reader **auto-enables every installed pack** so bilingual cascade works without a Settings visit.

The Dictionary entry only appears when at least one valid folder exists.

---

## Looking up words

### Enter dictionary mode

- Reader menu → **Dictionary**, or  
- **Settings → Controls → Long-press Menu** = Dictionary, then hold **Confirm** on the page.

Top title (reader chrome is hidden):

- **Dictionary Lookup** — default  
- **Multi-Word Selection** — after long-press **Select** to arm a range  

Move with Left/Right (and Up/Down by line). **Select** looks up. **Back** clears a multi-word range or exits.

### Lookup pipeline (per enabled pack)

1. **Normalize** — lowercase ASCII; keep Spanish accents; keep compound hyphens; strip quotes/punctuation/digits; join soft-/line-break hyphens; collapse spaces.
2. **Candidates**  
   - Single word: cleaned key, then English stems, Spanish clitics, hyphen splits, accent folding.  
   - Multi-word: full phrase → collocation windows (e.g. `por favor`) → each token with the same stem/clitic rules.  
3. **Index search** — binary search on `.qidx` samples, then short linear scan of `.idx` (buffered SD reads).  
4. **Definition** — read `.dict` range (or extract from `.dict.dz`).  
5. **Merge** — every pack that hits is shown under a **centered bold pack name** (`English`, `Spanish-English`, …).

### Definition card

- **Bold** headword; **regular** pronunciation beside it when present (`/…/`, `[…]`, or `(…)`).  
- Pack labels centered; senses left-aligned, numbered, capped for e-ink readability.  
- Drawn over a snapshot of the reader page (not a full white wipe when memory allows).

---

## Not supported / limitations

| Item | Behavior |
|------|----------|
| CXDict (`.cxdict`) | Not used by CrossPoint |
| `.idx.gz` | Must decompress first |
| `idxoffsetbits=64` | Rejected |
| `.syn` | Ignored |
| Full HTML definitions | Stripped / shown as simplified plain text |
| Arbitrary multi-word idioms | Only if they exist as headwords or are recoverable via windows/stems |

---

## Troubleshooting

| Symptom | Check |
|---------|--------|
| No Dictionary in Settings | Folders under `/dictionaries/` with one `.idx` + `.dict` each |
| Always “Not found” | Pack language mismatch; try Spanish-English for Spanish words; rebuild `.qidx` by deleting `*.qidx` |
| First lookup slow | Normal: building `.qidx` once per pack |
| Multi-word “not found” | Phrase may not be a headword; firmware still tries collocations and each word (e.g. `por favor`, `ayudar`) |
| Ambiguous folder skipped | More than one `.idx` stem inside the same folder |

For packaging notes shipped with the zip, see `dist/dictionaries/README.txt`.
