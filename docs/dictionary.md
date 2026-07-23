---
title: Dictionary
nav_order: 5
---

# Dictionary (English + Spanish)

Casper can look up words offline from dictionary packs on the SD card.

## Install dictionary files

Create this folder on the SD card:

```
/.crosspoint/dict/
```

Copy any of these files into it (use these exact names):

| File | Contents | Repo source |
|------|----------|-------------|
| `en.cxdict` | English **reader pack** (~150–250k headwords; Wiktionary-first, enPR, 2 senses) | `docs/en.cxdict` |
| `es-en.cxdict` | Spanish → English | `docs/es-en.cxdict` |
| `en-es.cxdict` | English → Spanish | `docs/en-es.cxdict` |
| `es.cxdict` | Spanish → Spanish (optional; not shipped yet) | build yourself |

You can install **one or more**. Lookup auto-tries installed packs in this order:

1. `en.cxdict` (EN)
2. `es.cxdict` (ES)
3. `es-en.cxdict` (ES→EN)
4. `en-es.cxdict` (EN→ES)

The popup shows which pack answered, e.g. `casa  [ES->EN]`.

### Recommended setup for bilingual reading

Copy all three shipped packs:

```
/.crosspoint/dict/en.cxdict
/.crosspoint/dict/es-en.cxdict
/.crosspoint/dict/en-es.cxdict
```

## Use it while reading

1. Open an EPUB.
2. Open the reader menu and choose **Dictionary** (sits above **Select Chapter**), or assign **Dictionary** to the power-button short/long press under **Settings → Controls → Power Button**.
3. Move the cursor onto a word (same navigation as clipping). Quotes and punctuation around the word are stripped automatically.
4. Press **Select** to open the definition popup. Book text stays visible around the card.
5. If several packs match (e.g. English **and** Spanish→English for `por`), the popup shows each under a `[EN]` / `[ES->EN]` section.
6. Inflected forms like *gagging* / *missiles* expand to include the base word’s meaning when available. Circular adverb glosses like *obsequiously* → “in an obsequious manner” also pull in the base adjective definition. English entries often include an IPA pronunciation (`/…/`).
7. Press **Select** again (**Close**) to dismiss the definition and keep choosing words.
8. Press **Back** to leave dictionary mode.
9. **Up/Down** scrolls long definitions while the popup is open.

**Spanish in books:** install `es-en.cxdict` (and optionally `es.cxdict`) next to `en.cxdict`. Without a Spanish pack, Spanish tokens only match accidental English hits (e.g. acronyms).

**Spanish clitics:** forms like *ayudame*, *dime*, *dámelo* are not always listed as headwords. The firmware strips object clitics (`me`, `te`, `se`, `lo`/`la`, …), looks up the stem or infinitive (`ayuda` / `ayudar`), and for ES→EN packs shows a natural English phrase on the first line when possible:

```
help me
ayuda (me / to me)
help, aid, assistance
```

Copy the rebuilt `docs/es-en.cxdict` to the SD card for this to work.

## Build packs yourself

English is built **Wiktionary-first**, with **Open English WordNet** (modern WordNet)
for gap-fill + short synonym hints, and public-domain Webster only for remaining holes:

```bash
# 1) Download English Wiktionary extract (~475 MB compressed) once:
#    https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl.gz
#    -> scripts/data/kaikki-en.jsonl.gz
#
# 2) OEWN JSON should live in scripts/data/oewn2025/ (see en-word.net)

# Compact reader pack (default): ~280k words, 2 senses, limited forms, enPR preferred.
# OEWN + Webster lemmas are always kept through the max-words trim (so literary
# words like "pendulous" are not dropped when Wiktionary alone would fill the budget).
python scripts/build_en_dict.py -o docs/en.cxdict

# Optional: full kitchen-sink pack
python scripts/build_en_dict.py -o docs/en-full.cxdict --full

# Optional TSV overrides (highest priority): word<TAB>definition
python scripts/build_en_dict.py -o docs/en.cxdict -i my_words.tsv
```

Spanish/English bilingual packs were converted from
[open-dsl-dict Wiktionary tabfiles](https://github.com/open-dsl-dict/wiktionary-dict)
(Creative Commons / GFDL). English monolingual uses
[kaikki.org English Wiktionary](https://kaikki.org/dictionary/English/) +
[Open English WordNet](https://en-word.net/) (CC-BY 4.0) + Webster gap-fill.

## Notes

- English + Spanish focus for this MVP.
- Simple stemming helps plurals / basic endings.
- Keys keep Spanish accents; a second pass folds accents if needed.
- Not yet: StarDict / reader.dict auto-import, full modern slang, UI language picker.
