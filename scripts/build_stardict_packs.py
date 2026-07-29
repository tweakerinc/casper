#!/usr/bin/env python3
"""Build clean Casper StarDict packs for ESP32-C3 e-readers.

Design goals (device stays thin):
  - Plain ASCII/UTF-8 headwords: no Wiktionary sort-key prefixes (8bay, Cbayed).
  - Sorted for case-insensitive binary search (matches firmware asciiCaseCmp).
  - Past/participle entries (bayed, lolled) are short, verb-first glosses of the
    base lemma — not full multi-homograph dumps of "bay".
  - sametypesequence=m plain text, 32-bit offsets, plain .dict (not .dict.dz).

Sources:
  EN:     CrossInk/Casper en.cxdict (Wiktionary + OEWN + Webster)
  EN-ES:  open-dsl-dict en-es.txt
  ES-EN:  open-dsl-dict es-en.txt

Usage:
  python scripts/build_stardict_packs.py \\
    --cxdict-dir E:/casper/docs \\
    --data-dir C:/Users/m/CrossInk/scripts/data \\
    -o C:/Users/m/Documents/Casper/dist/dictionaries
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

KEY_LEN = 40
CX_MAGIC = b"CXD1"
MAX_DEF_BYTES = 4096  # e-reader friendly; firmware allows 64KB but less is better

FORM_MARKERS = re.compile(
    r"(?is)(simple\s+past|past\s+participle|past\s+tense|present\s+participle|"
    r"third[-\s]person\s+singular)"
)
FORM_OF = re.compile(
    r"(?is)(?:simple\s+past|past\s+participle|past\s+tense|present\s+participle|"
    r"third[-\s]person\s+singular)\b[^.\n]{0,80}?\bof\s+([a-z][a-z\-']{1,30})"
)
POS_LINE = re.compile(r"^(noun|verb|adjective|adverb|pronoun|preposition|conjunction|"
                      r"interjection|article|phrase|idiom)\s*$", re.I)


# --- CXDict ------------------------------------------------------------------

def read_cxdict(path: Path) -> dict[str, str]:
    data = path.read_bytes()
    if data[:4] != CX_MAGIC:
        raise ValueError(f"{path}: bad magic {data[:4]!r}")
    wc, ioff, doff = struct.unpack_from("<III", data, 4)
    entries: dict[str, str] = {}
    pos = ioff
    for _ in range(wc):
        raw = data[pos : pos + KEY_LEN]
        key = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        off, ln = struct.unpack_from("<II", data, pos + KEY_LEN)
        pos += KEY_LEN + 8
        defn = data[doff + off : doff + off + ln].decode("utf-8", "replace")
        key = normalize_key(key)
        if key and defn:
            # Later duplicate keys keep longer definition.
            if key not in entries or len(defn) > len(entries[key]):
                entries[key] = defn
    return entries


# --- Bilingual tabfile -------------------------------------------------------

_TAB_RE = re.compile(
    r"^(?P<head>.+?)"
    r"(?:\s+\{(?P<pos>[^}]+)\})?"
    r"(?:\s+(?P<pron>/[^/]+/))?"
    r"(?:\s+\((?P<gloss>[^)]*)\))?"
    r"\s*::\s*"
    r"(?P<trans>.*)$"
)


def parse_tabfile(path: Path) -> dict[str, str]:
    buckets: dict[str, list[tuple[str, str, str, str]]] = {}
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = _TAB_RE.match(line)
            if not m:
                if " :: " not in line:
                    continue
                left, right = line.split(" :: ", 1)
                head = left.strip()
                key = normalize_key(head.split()[0] if head else "")
                if not key:
                    continue
                buckets.setdefault(key, []).append(("", "", "", right.strip()))
                continue
            head = m.group("head").strip()
            pos = (m.group("pos") or "").strip()
            pron = (m.group("pron") or "").strip()
            gloss = (m.group("gloss") or "").strip()
            trans = (m.group("trans") or "").strip()
            if not head or not trans:
                continue
            key = normalize_key(head)
            if not key:
                continue
            if " " in key and len(key) > 48:
                continue
            buckets.setdefault(key, []).append((pos, pron, gloss, trans))

    return {k: format_bilingual_def(v) for k, v in buckets.items()}


def expand_pos_label(pos: str) -> str:
    p = (pos or "").strip().lower()
    mapping = {
        "f": "feminine",
        "m": "masculine",
        "mf": "masc./fem.",
        "m/f": "masc./fem.",
        "f/m": "masc./fem.",
        "n": "noun",
        "v": "verb",
        "adj": "adjective",
        "adv": "adverb",
        "prep": "preposition",
        "conj": "conjunction",
        "pron": "pronoun",
        "interj": "interjection",
        "art": "article",
        "num": "numeral",
        "misc": "",
    }
    return mapping.get(p, pos.strip() if pos else "")


def format_bilingual_def(items: list[tuple[str, str, str, str]]) -> str:
    by_pos: dict[str, list[tuple[str, str, str]]] = {}
    first_pron = ""
    for pos, pron, gloss, trans in items:
        if pron and not first_pron:
            first_pron = pron
        p = pos.lower() if pos else "misc"
        by_pos.setdefault(p, []).append((pron, gloss, trans))

    lines: list[str] = []
    if first_pron:
        lines.append(first_pron)
    # Prefer verb then noun then rest (helps past-tense English readers less, but tidy).
    order = sorted(by_pos.keys(), key=lambda p: (0 if p in ("v", "verb") else 1 if p in ("n", "noun") else 2, p))
    for pos in order:
        senses = by_pos[pos]
        pos_label = expand_pos_label(pos)
        if pos_label:
            lines.append(pos_label)
        n = 1
        for _pron, gloss, trans in senses[:8]:
            body = trans
            if gloss:
                body = f"{trans} ({gloss})" if trans else gloss
            if not body or not body.strip():
                continue
            lines.append(f"{n}. {body.strip()}")
            n += 1
    return "\n".join(lines).strip()


# --- Key / definition cleaning -----------------------------------------------

def normalize_key(word: str) -> str:
    """Lowercase, strip punctuation/sort-key prefixes, keep hyphen/apostrophe inside."""
    w = word.strip()
    if not w:
        return ""
    # Drop Wiktionary/StarDict sort-key prefixes: "8bay", "Cbayed"
    if len(w) >= 2 and w[0].isdigit() and w[1].isalpha():
        w = w[1:]
    if len(w) >= 2 and w[0].isupper() and w[1].islower():
        # Only strip single uppercase if rest looks like a normal word (not "McDonald")
        if w[1:].islower() or (w[1].islower() and any(c.islower() for c in w[2:])):
            # Heuristic: strip only if first char is not part of common multi-cap patterns
            if not (w[0] in "IM" and len(w) <= 3):  # keep "I'm" handled below
                w = w[0].lower() + w[1:]
                # If original was Cbayed style (C + bayed all lower after), already lowercased C
                pass
    # Stronger: if key is like Cbayed (one upper + lower word), strip the upper
    if len(w) >= 3 and w[0].isupper() and w[1:].islower():
        w = w[1:]

    w = w.strip().lower()
    # Keep letters, digits, hyphen, apostrophe, space; drop other punctuation
    out = []
    for ch in w:
        o = ord(ch)
        if 97 <= o <= 122 or 48 <= o <= 57 or ch in "-' ":
            out.append(ch)
        elif o > 127:
            out.append(ch)  # keep Spanish accents etc.
    w = "".join(out)
    w = re.sub(r"\s+", " ", w).strip(" -'")
    # Final sort-key strip for digit/letter prefix remaining
    if len(w) >= 2 and w[0].isdigit() and w[1].isalpha():
        w = w[1:]
    return w


def strip_sort_key_aggressive(key: str) -> str:
    """Ensure index keys never carry 8bay / Cbayed prefixes."""
    k = key.strip()
    if len(k) >= 2 and k[0].isdigit() and (k[1].isalpha() or ord(k[1]) > 127):
        k = k[1:]
    if len(k) >= 2 and "A" <= k[0] <= "Z" and ("a" <= k[1] <= "z" or ord(k[1]) > 127):
        k = k[1:]
    return k.lower().strip()


def parse_en_sections(defn: str) -> list[tuple[str, list[str]]]:
    """Split EN def into [(pos, [sense_lines]), ...]. Pron kept on empty pos ''."""
    sections: list[tuple[str, list[str]]] = []
    cur_pos = ""
    cur_senses: list[str] = []
    first_pron = ""

    def flush():
        nonlocal cur_senses
        if cur_pos or cur_senses:
            sections.append((cur_pos, cur_senses))
        cur_senses = []

    for raw in defn.split("\n"):
        s = raw.strip()
        if not s:
            continue
        if (s.startswith("/") and s.endswith("/")) or (s.startswith("[") and s.endswith("]")):
            if not first_pron:
                first_pron = s
            continue
        if s.startswith("(") and s.endswith(")") and len(s) < 40:
            if not first_pron:
                first_pron = s
            continue
        if POS_LINE.match(s):
            flush()
            cur_pos = s.lower()
            continue
        # Drop recursive embedded headword lines that restart the article
        if s.isalpha() and len(s) < 24 and not s[0].isupper():
            # bare "bay" mid-article (lemma restatement) — skip as section noise
            if sections or cur_senses:
                continue
        # Numbered or bare sense
        m = re.match(r"^\d+[\.\)]\s*(.*)$", s)
        sense = m.group(1).strip() if m else s
        if sense:
            cur_senses.append(sense)
    flush()

    if first_pron:
        sections.insert(0, ("__pron__", [first_pron]))
    return sections


def rebuild_from_sections(sections: list[tuple[str, list[str]]], max_senses: int = 6) -> str:
    lines: list[str] = []
    sense_total = 0
    # Verb sections first for cleaner past-tense stems of multi-homograph lemmas.
    order = [s for s in sections if s[0] == "verb"] + [s for s in sections if s[0] not in ("verb", "__pron__")]
    pron = [s for s in sections if s[0] == "__pron__"]
    for pos, senses in pron:
        lines.extend(senses)
    for pos, senses in order:
        if not senses:
            continue
        if pos and pos != "__pron__":
            lines.append(pos)
        n = 1
        for sense in senses:
            if sense_total >= max_senses:
                break
            # Skip pure form lines when we have real content
            if FORM_MARKERS.search(sense) and " of " in sense.lower() and len(sense) < 100:
                continue
            lines.append(f"{n}. {sense}")
            n += 1
            sense_total += 1
        if sense_total >= max_senses:
            break
    return "\n".join(lines).strip()


def is_pure_form_stub(defn: str) -> bool:
    if len(defn) > 280:
        return False
    if not FORM_MARKERS.search(defn[:160] or ""):
        return False
    # Short + form marker near start
    return len(defn) <= 200 or defn.count("\n") <= 5


def verb_senses_from_lemma(lemma_def: str, limit: int = 3) -> list[str]:
    sections = parse_en_sections(lemma_def)
    out: list[str] = []
    for pos, senses in sections:
        if pos != "verb":
            continue
        for s in senses:
            if FORM_MARKERS.search(s) and " of " in s.lower():
                continue
            out.append(s)
            if len(out) >= limit:
                return out
    # Fall back to first non-form senses if no verb block
    if not out:
        for pos, senses in sections:
            if pos in ("__pron__",):
                continue
            for s in senses:
                if FORM_MARKERS.search(s) and " of " in s.lower():
                    continue
                out.append(s)
                if len(out) >= limit:
                    return out
    return out


def clean_en_entries(raw: dict[str, str]) -> dict[str, str]:
    # Pass 1: normalize keys, light def cleanup
    base: dict[str, str] = {}
    for k, v in raw.items():
        key = strip_sort_key_aggressive(normalize_key(k))
        if not key or len(key) > 48:
            continue
        defn = v.replace("\r\n", "\n").replace("\r", "\n").strip()
        if not defn:
            continue
        if key not in base or len(defn) > len(base[key]):
            base[key] = defn

    # Pass 2: rebuild form-of past/participle entries from lemma verb senses
    cleaned: dict[str, str] = {}
    for key, defn in base.items():
        form = FORM_OF.search(defn[:220]) if FORM_MARKERS.search(defn[:160] or "") else None
        lemma = form.group(1).lower() if form else None
        if lemma:
            lemma = strip_sort_key_aggressive(normalize_key(lemma))

        looks_inflected = key.endswith("ed") or key.endswith("ing") or key.endswith("es") or (
            key.endswith("s") and len(key) > 4
        )

        if lemma and lemma in base and (is_pure_form_stub(defn) or looks_inflected):
            # Short, reader-friendly form entry: state the form, then lemma verb glosses.
            senses = verb_senses_from_lemma(base[lemma], limit=3)
            if key.endswith("ing"):
                form_line = f"Present participle / -ing form of {lemma}."
            elif key.endswith("ed") or key == "went" or key.endswith("en"):
                form_line = f"Past tense / past participle of {lemma}."
            else:
                form_line = f"Inflected form of {lemma}."
            lines = ["verb", form_line]
            for i, s in enumerate(senses, 1):
                # Drop recursive "… of lemma" noise; keep real glosses (bark/howl).
                if FORM_MARKERS.search(s) and " of " in s.lower():
                    continue
                lines.append(f"{i}. {s}")
            # Renumber after skips
            senses_out = [ln for ln in lines if ln[:1].isdigit()]
            if not senses_out:
                for s in verb_senses_from_lemma(base[lemma], limit=3):
                    if not (FORM_MARKERS.search(s) and " of " in s.lower()):
                        lines.append(f"1. {s}")
                        break
                if len([ln for ln in lines if ln[:1].isdigit()]) == 0:
                    first = next(
                        (
                            ln.strip()
                            for ln in base[lemma].split("\n")
                            if ln.strip()
                            and not ln.strip().startswith("/")
                            and not POS_LINE.match(ln.strip())
                            and not (FORM_MARKERS.search(ln) and " of " in ln.lower())
                        ),
                        "",
                    )
                    if first:
                        lines.append(f"1. {first}")
            # Re-number sense lines cleanly
            rebuilt = ["verb", form_line]
            n = 1
            for ln in lines[2:]:
                m = re.match(r"^\d+[\.\)]\s*(.*)$", ln)
                body = m.group(1).strip() if m else ln.strip()
                if not body:
                    continue
                rebuilt.append(f"{n}. {body}")
                n += 1
            cleaned[key] = "\n".join(rebuilt)
            continue

        # Normal entry: reorder verb first, cap senses, drop form-only noise
        sections = parse_en_sections(defn)
        rebuilt = rebuild_from_sections(sections, max_senses=6)
        if not rebuilt:
            rebuilt = defn.strip()
        if len(rebuilt.encode("utf-8")) > MAX_DEF_BYTES:
            rebuilt = rebuilt.encode("utf-8")[:MAX_DEF_BYTES].decode("utf-8", "ignore").rsplit("\n", 1)[0]
        cleaned[key] = rebuilt

    return cleaned


def clean_bilingual_entries(raw: dict[str, str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for k, v in raw.items():
        key = strip_sort_key_aggressive(normalize_key(k))
        if not key or len(key) > 48:
            continue
        defn = v.strip()
        if not defn:
            continue
        if len(defn.encode("utf-8")) > MAX_DEF_BYTES:
            defn = defn.encode("utf-8")[:MAX_DEF_BYTES].decode("utf-8", "ignore")
        if key not in out or len(defn) > len(out[key]):
            out[key] = defn
    return out


# --- StarDict writer ---------------------------------------------------------

def stardict_key(s: str) -> str:
    return s.lower()


def write_stardict(out_dir: Path, stem: str, bookname: str, entries: dict[str, str], description: str) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    items = sorted(
        ((k, v) for k, v in entries.items() if k and v),
        key=lambda kv: (stardict_key(kv[0]), kv[0]),
    )
    # Verify no sort-key leftovers
    bad = [k for k, _ in items if (k[:1].isdigit() and len(k) > 1) or (len(k) > 1 and k[0].isupper())]
    if bad[:5]:
        print(f"  WARN: possible sort-key keys sample: {bad[:5]}")

    dict_parts: list[bytes] = []
    idx_parts: list[bytes] = []
    offset = 0
    for word, defn in items:
        body = defn.encode("utf-8", "replace")
        if len(body) > 60000:
            body = body[:60000]
        dict_parts.append(body)
        wbytes = word.encode("utf-8", "replace")
        idx_parts.append(wbytes + b"\0" + struct.pack(">II", offset, len(body)))
        offset += len(body)

    dict_data = b"".join(dict_parts)
    idx_data = b"".join(idx_parts)
    (out_dir / f"{stem}.dict").write_bytes(dict_data)
    (out_dir / f"{stem}.idx").write_bytes(idx_data)
    ifo = (
        "StarDict's dict ifo file\n"
        "version=2.4.2\n"
        f"wordcount={len(items)}\n"
        f"idxfilesize={len(idx_data)}\n"
        f"bookname={bookname}\n"
        f"description={description}\n"
        "sametypesequence=m\n"
    )
    (out_dir / f"{stem}.ifo").write_text(ifo, encoding="utf-8")
    # Drop device-built index so it rebuilds against the new pack
    qidx = out_dir / f"{stem}.qidx"
    if qidx.exists():
        qidx.unlink()
    print(f"Wrote {out_dir}  words={len(items)}  dict={len(dict_data)/1e6:.1f}MB  idx={len(idx_data)/1e6:.2f}MB")


def verify_samples(en: dict[str, str]) -> None:
    for k in ("bay", "bayed", "loll", "lolled", "run", "running", "go", "went"):
        if k not in en:
            print(f"  VERIFY miss: {k}")
            continue
        snippet = en[k].replace("\n", " | ")[:180]
        print(f"  VERIFY {k}: {snippet}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxdict-dir", type=Path, default=Path(r"E:\casper\docs"))
    ap.add_argument("--data-dir", type=Path, default=Path(r"C:\Users\m\CrossInk\scripts\data"))
    ap.add_argument("-o", "--out", type=Path, default=Path(r"C:\Users\m\Documents\Casper\dist\dictionaries"))
    args = ap.parse_args()

    # Fallbacks for sources
    if not (args.cxdict_dir / "en.cxdict").exists():
        for p in (Path(r"C:\Users\m\CrossInk\docs"), Path(r"E:\casper\docs")):
            if (p / "en.cxdict").exists():
                args.cxdict_dir = p
                break
    if not (args.data_dir / "en-es.txt").exists():
        for p in (Path(r"C:\Users\m\CrossInk\scripts\data"), Path(r"E:\casper\scripts\data")):
            if (p / "en-es.txt").exists():
                args.data_dir = p
                break

    en_cx = args.cxdict_dir / "en.cxdict"
    if not en_cx.exists():
        print("ERROR: missing en.cxdict in", args.cxdict_dir, file=sys.stderr)
        return 1

    print("Loading EN from", en_cx)
    en_raw = read_cxdict(en_cx)
    print(f"  raw entries: {len(en_raw)}")
    en = clean_en_entries(en_raw)
    print(f"  cleaned entries: {len(en)}")
    verify_samples(en)
    write_stardict(
        args.out / "English",
        "english",
        "Casper English (clean)",
        en,
        "Clean English monolingual for Casper. Plain keys, verb-first multi-sense, "
        "short past-tense forms. Sources: Wiktionary + WordNet + Webster (via CXDict).",
    )

    en_es_tab = args.data_dir / "en-es.txt"
    en_es_cx = args.cxdict_dir / "en-es.cxdict"
    if en_es_tab.exists():
        print("Loading EN-ES from", en_es_tab)
        en_es = clean_bilingual_entries(parse_tabfile(en_es_tab))
    elif en_es_cx.exists():
        print("Loading EN-ES from", en_es_cx)
        en_es = clean_bilingual_entries(read_cxdict(en_es_cx))
    else:
        print("WARN: no EN-ES source", file=sys.stderr)
        en_es = {}
    if en_es:
        write_stardict(
            args.out / "English-Spanish",
            "english-spanish",
            "Casper English-Spanish (clean)",
            en_es,
            "English to Spanish. Wiktionary open-dsl-dict (CC BY-SA / GFDL). Plain keys.",
        )

    es_en_tab = args.data_dir / "es-en.txt"
    es_en_cx = args.cxdict_dir / "es-en.cxdict"
    if es_en_tab.exists():
        print("Loading ES-EN from", es_en_tab)
        es_en = clean_bilingual_entries(parse_tabfile(es_en_tab))
    elif es_en_cx.exists():
        print("Loading ES-EN from", es_en_cx)
        es_en = clean_bilingual_entries(read_cxdict(es_en_cx))
    else:
        print("WARN: no ES-EN source", file=sys.stderr)
        es_en = {}
    if es_en:
        write_stardict(
            args.out / "Spanish-English",
            "spanish-english",
            "Casper Spanish-English (clean)",
            es_en,
            "Spanish to English. Wiktionary open-dsl-dict (CC BY-SA / GFDL). Plain keys.",
        )

    # Also write under .dictionaries for hidden install
    hidden = args.out.parent / ".dictionaries"
    if args.out.name == "dictionaries":
        import shutil

        if hidden.exists():
            shutil.rmtree(hidden)
        shutil.copytree(args.out, hidden)
        print("Mirrored to", hidden)

    readme = args.out / "README.txt"
    readme.write_text(
        """Casper clean StarDict packs
===========================

Copy folders to the SD card:

  /dictionaries/English/
  /dictionaries/English-Spanish/
  /dictionaries/Spanish-English/

Or hide them:

  /.dictionaries/English/
  ...

On device: Settings -> Reader -> Dictionary -> multi-select packs -> Save.

These packs are CLEANED for Casper:
  - Plain headwords (no 8bay / Cbayed sort keys)
  - Case-insensitive sort for firmware binary search
  - bayed / lolled etc. are short past-tense verb entries, not full dumps of bay
  - Plain .dict (faster than .dict.dz)
  - sametypesequence=m, 32-bit offsets only

First lookup builds .qidx on the SD card (once per pack).

Sources: Wiktionary + WordNet + Webster (EN); Wiktionary open-dsl-dict (bilingual).
""",
        encoding="utf-8",
    )
    print("Done. Output:", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
