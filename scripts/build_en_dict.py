#!/usr/bin/env python3
"""Build a high-quality CrossInk English dictionary (.cxdict).

Priority (definition quality first):
  1) English Wiktionary via kaikki.org extract (modern, readable glosses)
  2) Open English WordNet 2025 (short clean senses for gaps)
  3) Optional public-domain Webster JSON (last resort gaps only)
  4) Optional TSV merge

Format (little-endian):
  Header 16 bytes: magic 'CXD1' | wordCount u32 | indexOffset u32 | dataOffset u32
  Index: wordCount * (key[40] + offset u32 + length u32)
  Data: packed UTF-8 definitions (max 1024 bytes each; may include /IPA/ + form-of expansion)

Examples:
  # Badass default: local kaikki.gz + OEWN (+ Webster gaps)
  python scripts/build_en_dict.py -o docs/en.cxdict

  # Wiktionary only
  python scripts/build_en_dict.py -o docs/en.cxdict --no-oewn --no-webster

Copy to SD: /.crosspoint/dict/en.cxdict
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import struct
import sys
from pathlib import Path

MAGIC = b"CXD1"
KEY_LEN = 40
MAX_DEF_BYTES = 1024
DEFAULT_DATA = Path(__file__).resolve().parent / "data"
DEFAULT_WEBSTER = DEFAULT_DATA / "en_dictionary.json"
DEFAULT_OEWN = DEFAULT_DATA / "oewn2025"
DEFAULT_KAIKKI_GZ = DEFAULT_DATA / "kaikki-en.jsonl.gz"
DEFAULT_KAIKKI_JSONL = DEFAULT_DATA / "kaikki-en.jsonl"

# Skip low-value glosses for a reader dictionary.
_SKIP_GLOSS_RE = re.compile(
    r"^(?:"
    r"obsolete (?:form|spelling) of|"
    r"archaic (?:form|spelling) of|"
    r"misspelling of|"
    r"alternative (?:form|spelling|letter-case form) of|"
    r"alternative letter-case form of|"
    r"nonstandard form of|"
    r"eye dialect (?:spelling )?of|"
    r"pronunciation spelling of|"
    r"soft mutation of|"
    r"eclipsis of|"
    r"h-prothesis of|"
    r"initialism of|"
    r"acronym of|"
    r"abbreviation of|"
    r"clipping of|"
    r"ellipsis of|"
    r"symbol (?:for|of)|"
    r"only used in|"
    r"used only in|"
    r"a spelling of|"
    r"(?:British|American|Canadian|Australian) spelling of"
    r")\b",
    re.I,
)

# Still useful as fallback when no real gloss exists (inflections).
_FORM_OF_RE = re.compile(
    r"^(?:"
    r"plural of|"
    r"present participle(?: and gerund)? of|"
    r"present participle and gerund of|"
    r"simple past(?: tense and past participle)? of|"
    r"past participle of|"
    r"third-person singular simple present indicative form of|"
    r"comparative form of|"
    r"superlative form of|"
    r"adverb(?:ial)?(?:\s+form)? of"
    r")\b",
    re.I,
)

# Capture base lemma from inflectional glosses.
_FORM_OF_BASE_RE = re.compile(
    r"(?i)\b(?:plural|participle|gerund|past(?:\s+participle)?|form|tense|indicative|"
    r"comparative|superlative|adverb(?:ial)?)\b"
    r".*?\bof\s+([A-Za-z][A-Za-z'’\-]{1,38})"
)

# WordNet/Wiktionary adverbs often say only "in an X manner" — useless alone.
# Capture the adjective base so we can append its real definition.
_MANNER_ADV_BASE_RE = re.compile(
    r"(?i)\bin\s+an?\s+([A-Za-z][A-Za-z'’\-]{1,38})\s+(?:manner|way|fashion)\b"
)


def normalize_key(word: str) -> str:
    out = []
    for ch in word.strip().lower():
        if "a" <= ch <= "z":
            out.append(ch)
    return "".join(out)[: KEY_LEN - 1]


# IPA / fancy punctuation → ASCII so e-ink UI fonts don't show tofu glyphs.
# Spanish áéíóúüñ are kept (common in the UI font). Firmware also folds at lookup.
_IPA_ASCII_MAP = {
    "ə": "uh",
    "ɚ": "uh",
    "ɜ": "uh",
    "ɝ": "uh",
    "ɪ": "i",
    "ʊ": "u",
    "ʌ": "u",
    "ɑ": "a",
    "ɒ": "a",
    "ɔ": "o",
    "ɛ": "e",
    "æ": "ae",
    "ʃ": "sh",
    "ʒ": "zh",
    "ɹ": "r",
    "ɡ": "g",
    "ŋ": "ng",
    "θ": "th",
    "ð": "th",
    "ˈ": "'",
    "ˌ": ",",
    "ː": ":",
    "ʔ": "'",
    "“": '"',
    "”": '"',
    "‘": "'",
    "’": "'",
    "–": "-",
    "—": "-",
    "…": "...",
    "†": "*",
    "‡": "*",
    "·": "-",
    "\u00a0": " ",
}


def fold_for_ui_font(text: str) -> str:
    """Approximate IPA/punctuation for fonts without those glyphs."""
    if not text:
        return text
    keep = set("áéíóúüñÁÉÍÓÚÜÑ")
    out: list[str] = []
    for ch in text:
        o = ord(ch)
        # Drop combining marks
        if 0x300 <= o <= 0x36F or 0x1AB0 <= o <= 0x1AFF or 0x1DC0 <= o <= 0x1DFF:
            continue
        if 0x2070 <= o <= 0x209F:  # super/sub scripts
            continue
        if ch in _IPA_ASCII_MAP:
            out.append(_IPA_ASCII_MAP[ch])
            continue
        if o < 128 or ch in keep:
            out.append(ch)
            continue
        # else drop (no tofu)
    return "".join(out)


def clean_def_preserve_newlines(text: str) -> str:
    """Keep single newlines between layout lines (IPA / POS / senses)."""
    text = fold_for_ui_font(text)
    lines = []
    for line in text.replace("\r", "\n").split("\n"):
        line = " ".join(line.split())
        if line:
            lines.append(line)
    text = "\n".join(lines)
    raw = text.encode("utf-8")
    if len(raw) <= MAX_DEF_BYTES:
        return text
    cut = raw[:MAX_DEF_BYTES].decode("utf-8", errors="ignore")
    if "\n" in cut:
        cut = cut.rsplit("\n", 1)[0]
    else:
        cut = cut.rsplit(" ", 1)[0] + "..."
    return cut


def clean_def(text: str) -> str:
    # Preserve newlines used for Webster-style layout (IPA / POS / senses).
    if "\n" in text:
        return clean_def_preserve_newlines(text)
    text = fold_for_ui_font(text)
    text = " ".join(text.replace("\t", " ").replace("\r", " ").split())
    if not text:
        return ""
    raw = text.encode("utf-8")
    if len(raw) <= MAX_DEF_BYTES:
        return text
    cut = raw[:MAX_DEF_BYTES].decode("utf-8", errors="ignore").rstrip()
    if len(cut) > 3:
        cut = cut[:-1].rsplit(" ", 1)[0] + "..."
    return cut


def is_single_token_word(word: str) -> bool:
    if not word or " " in word or "_" in word or "/" in word:
        return False
    # Allow hyphen/apostrophe in source; normalize_key strips them.
    letters = sum(1 for c in word if ("a" <= c.lower() <= "z"))
    # Allow single-letter function words (a, i); other 1-letter noise still filtered
    # by callers / CORE_FUNCTION_WORDS seeding.
    return letters >= 1


# Always force these into the reader pack (trim must not drop them; kaikki may
# skip single letters or odd POS layouts for ultra-common words like "and").
CORE_FUNCTION_WORDS: dict[str, str] = {
    "a": "article\n• used before a singular noun when not specific\n• one; any",
    "an": "article\n• form of a used before a vowel sound",
    "and": "conjunction\n• connects words or clauses; also; plus\n• then; next in a sequence",
    "i": "pronoun\n• the speaker or writer; first person singular",
    "because": "conjunction\n• for the reason that; since",
    "against": "preposition\n• in opposition to\n• next to; touching",
}


def gloss_quality(gloss: str) -> int:
    """Higher is better. Used when merging senses."""
    g = gloss.strip()
    if not g or len(g) < 3:
        return -100
    low = g.lower()
    if _SKIP_GLOSS_RE.match(low):
        return -50
    if _FORM_OF_RE.match(low):
        return 10  # usable for inflections, not preferred
    score = 50
    # Prefer concise reader defs.
    if 12 <= len(g) <= 220:
        score += 20
    elif len(g) > 400:
        score -= 15
    if g[0].isupper() or g[0].islower():
        score += 2
    if "\n" in g:
        score -= 5
    return score


# Reader pack defaults: short, clean, UI-font-safe.
DEFAULT_MAX_SENSES = 2
DEFAULT_MAX_FORMS_PER_HEAD = 6
# Reader pack: room for OEWN+Webster must-keeps plus a large Wiktionary head set.
DEFAULT_MAX_WORDS = 280_000


def pick_best_gloss_list(senses: list, max_senses: int = DEFAULT_MAX_SENSES) -> list[str]:
    """Return ordered gloss strings (best first), not joined."""
    scored: list[tuple[int, str]] = []
    for sense in senses:
        if not isinstance(sense, dict):
            continue
        # Skip proper-name / rare sense tags when possible
        tags = [str(t).lower() for t in (sense.get("tags") or [])]
        if any(t in tags for t in ("obsolete", "rare", "archaic", "misspelling")):
            continue
        for field in ("glosses", "raw_glosses"):
            gl = sense.get(field) or []
            if not gl:
                continue
            g = str(gl[0]).strip()
            q = gloss_quality(g)
            if q < 0 and not _FORM_OF_RE.match(g):
                break
            scored.append((q, g))
            break
    if not scored:
        return []
    scored.sort(key=lambda t: t[0], reverse=True)
    real = [g for q, g in scored if q >= 40]
    chosen = real[:max_senses] if real else [g for _, g in scored[:1]]
    out: list[str] = []
    seen = set()
    for g in chosen:
        k = g.lower()
        if k in seen:
            continue
        seen.add(k)
        out.append(g)
    return out


def pick_best_glosses(senses: list, max_senses: int = DEFAULT_MAX_SENSES) -> str:
    # Back-compat helper (plain join). Prefer format_webster_entry for packs.
    glosses = pick_best_gloss_list(senses, max_senses=max_senses)
    return clean_def("; ".join(glosses)) if glosses else ""


_POS_LABEL = {
    "noun": "noun",
    "name": "noun",
    "prop_noun": "noun",
    "verb": "verb",
    "adj": "adjective",
    "adj_noun": "adjective",
    "adv": "adverb",
    "prep": "preposition",
    "conj": "conjunction",
    "pron": "pronoun",
    "det": "determiner",
    "article": "article",
    "num": "numeral",
    "intj": "interjection",
    "particle": "particle",
    "suffix": "suffix",
    "prefix": "prefix",
    "infix": "infix",
    "abbrev": "abbreviation",
    "phrase": "phrase",
}


def pos_label(pos: str | None) -> str:
    if not pos:
        return ""
    return _POS_LABEL.get(str(pos).strip().lower(), str(pos).strip().lower())


def format_webster_entry(pos: str | None, glosses: list[str], pron: str = "") -> str:
    """Reader layout for small e-ink popups (firmware also normalizes older packs):

      (MIS-uhl)
      noun
      • an object thrown or projected …
      • a self-propelled projectile …

    Pack labels (EN / ES->EN) are added at lookup time, not stored in the pack.
    Pronunciation prefers American enPR (ASCII-friendly) over IPA.
    """
    if not glosses:
        return ""
    lines: list[str] = []
    if pron:
        p = pron.strip()
        if p and not (p.startswith("(") or p.startswith("/")):
            p = f"({p})"
        if p:
            lines.append(p)
    label = pos_label(pos)
    if label:
        lines.append(label)

    def colonize(g: str) -> str:
        g = " ".join(g.split())
        if not g:
            return g
        if _FORM_OF_RE.match(g):
            return g
        if g[0].isupper() and (len(g) == 1 or not g[1].isupper()):
            g = g[0].lower() + g[1:]
        return g

    for g in glosses:
        # Form-of stays unbulleted so firmware can treat it as context.
        cg = colonize(g)
        if _FORM_OF_RE.match(cg):
            lines.append(cg)
        else:
            lines.append(f"• {cg}")
    return clean_def_preserve_newlines("\n".join(lines))


def merge_webster_blocks(existing: str, new_block: str) -> str:
    """Merge another POS block into an existing entry without duplicating IPA."""
    if not existing:
        return new_block
    if not new_block:
        return existing
    # If new block's POS line already present, keep longer block.
    new_lines = new_block.split("\n")
    ex_lines = existing.split("\n")
    # Drop leading IPA from new if existing already has IPA.
    if new_lines and new_lines[0].startswith("/") and ex_lines and ex_lines[0].startswith("/"):
        new_lines = new_lines[1:]
    candidate = existing.rstrip() + "\n" + "\n".join(new_lines)
    return clean_def_preserve_newlines(candidate)


def extract_pronunciation(obj: dict) -> str:
    """Prefer American enPR (desk-dictionary style), else IPA (folded later).

    enPR examples: "dĭk'shə-nĕr-ē" — more readable on e-ink after ASCII fold.
    """
    sounds = obj.get("sounds") or []
    if not isinstance(sounds, list):
        return ""
    first_enpr = ""
    first_ipa = ""
    for s in sounds:
        if not isinstance(s, dict):
            continue
        tags = [str(t) for t in (s.get("tags") or [])]
        tag_l = " ".join(tags).lower()
        is_us = "us" in tag_l or "general-american" in tag_l or "general american" in tag_l

        enpr = s.get("enpr")
        if isinstance(enpr, str) and enpr.strip():
            enpr = fold_for_ui_font(enpr.strip())
            enpr = " ".join(enpr.split())
            if 2 <= len(enpr) <= 40:
                if is_us:
                    return enpr
                if not first_enpr:
                    first_enpr = enpr

        ipa = s.get("ipa")
        if isinstance(ipa, str) and ipa.strip():
            ipa = ipa.strip()
            if not ipa.startswith("/"):
                ipa = "/" + ipa.strip("/") + "/"
            ipa = fold_for_ui_font(ipa)
            if 2 <= len(ipa) <= 40:
                if is_us and not first_enpr:
                    first_ipa = ipa
                elif not first_ipa:
                    first_ipa = ipa
    return first_enpr or first_ipa


def extract_expansion_base(defn: str) -> str | None:
    """Return base lemma from form-of or 'in an X manner/way/fashion' glosses."""
    if not defn:
        return None
    # Prefer explicit form-of ("adverb of X", "plural of X").
    m = _FORM_OF_BASE_RE.search(defn)
    if m:
        return normalize_key(m.group(1)) or None
    # Circular adverb glosses: "in an obsequious manner."
    m = _MANNER_ADV_BASE_RE.search(defn)
    if m:
        return normalize_key(m.group(1)) or None
    return None


def expand_form_of_entries(entries: dict[str, str]) -> int:
    """Append the base lemma's definition for form-of / manner-only adverbs.

    Examples:
      'plural of missile' + missile def
      'in an obsequious manner' + obsequious def  (otherwise useless alone)
    """
    expanded = 0
    # Snapshot so we always expand against original base defs.
    base_snapshot = dict(entries)
    for key, defn in list(entries.items()):
        # Long entries that already look like real multi-sense defs can skip
        # form-of matching, but still expand pure "in an X manner" adverbs.
        manner_hit = _MANNER_ADV_BASE_RE.search(defn) is not None
        if len(defn) > 140 and not _FORM_OF_RE.match(defn) and not manner_hit:
            continue
        base = extract_expansion_base(defn)
        if not base or base == key or base not in base_snapshot:
            continue
        base_def = base_snapshot[base]
        # Skip if base is also pure form-of noise.
        if _FORM_OF_RE.match(base_def) and len(base_def) < 80:
            b2 = extract_expansion_base(base_def)
            if b2 and b2 in base_snapshot and not _FORM_OF_RE.match(base_snapshot[b2]):
                base = b2
                base_def = base_snapshot[b2]
        # Already expanded? (base heading or "base:" inline)
        low = defn.lower()
        if f"\n{base}\n" in low or low.startswith(f"{base}\n") or f"{base}:" in low or f"{base} :" in low:
            continue
        # For manner glosses, only expand when the entry is mostly circular
        # (no substantial second sense). Keeps "happily" style multi-glosses lean.
        if manner_hit and not _FORM_OF_RE.search(defn):
            # Strip POS/pron lines and bullets; measure remaining "real" content.
            body_lines = []
            for line in defn.split("\n"):
                s = line.strip()
                if not s:
                    continue
                if s.startswith("/") or (s.startswith("(") and s.endswith(")")):
                    continue
                if s.lower() in (
                    "noun",
                    "verb",
                    "adjective",
                    "adverb",
                    "pronoun",
                    "preposition",
                    "conjunction",
                    "interjection",
                    "determiner",
                    "numeral",
                    "particle",
                ):
                    continue
                body_lines.append(s.lstrip("•:- ").strip())
            body = " ".join(body_lines)
            # If more than one substantial clause beyond the manner phrase, skip.
            if len(body) > 90 and ";" in body:
                continue
            if len(body) > 120:
                continue
        core = defn.rstrip(" .;")
        # Keep layout readable: original gloss, then base entry block.
        new = clean_def_preserve_newlines(f"{core}\n\n{base}\n{base_def}")
        if new != defn:
            entries[key] = new
            expanded += 1
    print(f"form-of/manner expand: {expanded} entries")
    return expanded


def add_entry(
    entries: dict[str, str],
    word: str,
    definition: str,
    *,
    priority: int,
    priorities: dict[str, int],
    merge_same_priority: bool = False,
) -> bool:
    """Insert/replace if this source priority is higher, or equal with better gloss."""
    if not is_single_token_word(word):
        return False
    key = normalize_key(word)
    if len(key) < 1:
        return False
    # Allow single-letter only for known function words (a, i).
    if len(key) < 2 and key not in CORE_FUNCTION_WORDS:
        return False
    defn = clean_def(definition)
    if not defn:
        return False
    prev_p = priorities.get(key, -1)
    if key not in entries:
        entries[key] = defn
        priorities[key] = priority
        return True
    if priority > prev_p:
        entries[key] = defn
        priorities[key] = priority
        return True
    if priority == prev_p:
        if merge_same_priority:
            # e.g. noun block + verb block for the same spelling.
            merged = merge_webster_blocks(entries[key], defn)
            if merged != entries[key]:
                entries[key] = merged
                return True
            return False
        if gloss_quality(defn) > gloss_quality(entries[key]):
            entries[key] = defn
            return True
    return False


def load_kaikki(
    path: Path,
    priorities: dict[str, int],
    entries: dict[str, str],
    *,
    max_senses: int = DEFAULT_MAX_SENSES,
    max_forms_per_head: int = DEFAULT_MAX_FORMS_PER_HEAD,
) -> None:
    if not path.is_file():
        print(f"warn: kaikki file missing: {path}", file=sys.stderr)
        return

    open_fn = gzip.open if path.suffix == ".gz" or path.name.endswith(".jsonl.gz") else open
    mode = "rt"
    print(f"kaikki: loading {path.name} (max_senses={max_senses}, max_forms={max_forms_per_head}) ...", flush=True)
    lines = 0
    heads = 0
    forms_added = 0
    # Common inflection tags only (keeps pack lean).
    useful_form_tags = {
        "plural",
        "present",
        "past",
        "participle",
        "gerund",
        "third-person",
        "singular",
        "comparative",
        "superlative",
        "adverb",
        "adverbial",
        "adverb-form",
    }
    with open_fn(path, mode, encoding="utf-8", errors="replace") as fh:  # type: ignore[arg-type]
        for line in fh:
            lines += 1
            if lines % 100000 == 0:
                print(f"  ... lines={lines} entries={len(entries)}", flush=True)
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            lang = obj.get("lang_code") or obj.get("lang")
            if lang not in (None, "en", "English"):
                continue

            word = obj.get("word")
            if not isinstance(word, str):
                continue
            # Skip multi-word and very long headwords for a reader pack.
            if " " in word or len(word) > 28:
                continue

            pos = obj.get("pos")
            pos_s = pos if isinstance(pos, str) else ""
            # Skip pure names / symbols for the compact reader pack.
            if pos_s in ("name", "symbol", "character", "suffix", "prefix", "infix", "diacritical_mark"):
                continue

            senses = obj.get("senses") or []
            glosses = pick_best_gloss_list(senses, max_senses=max_senses)
            if not glosses:
                continue

            pron = extract_pronunciation(obj)
            head_defn = format_webster_entry(pos_s or None, glosses, pron=pron)
            if not head_defn:
                continue

            if add_entry(
                entries, word, head_defn, priority=100, priorities=priorities, merge_same_priority=True
            ):
                heads += 1

            # Limited useful inflections only (stemming covers the rest at lookup).
            form_count = 0
            for form_obj in obj.get("forms") or []:
                if form_count >= max_forms_per_head:
                    break
                if not isinstance(form_obj, dict):
                    continue
                form = form_obj.get("form")
                if not isinstance(form, str) or " " in form:
                    continue
                tags = {str(t).lower() for t in (form_obj.get("tags") or [])}
                if tags & {"romanization", "obsolete", "rare", "archaic", "misspelling"}:
                    continue
                if tags and not (tags & useful_form_tags):
                    continue
                if add_entry(entries, form, head_defn, priority=90, priorities=priorities):
                    forms_added += 1
                    form_count += 1

    print(f"kaikki: lines={lines} head_adds~={heads} form_adds~={forms_added} total={len(entries)}")


def trim_to_max_words(
    entries: dict[str, str],
    priorities: dict[str, int],
    max_words: int,
    *,
    core_lemmas: set[str] | None = None,
) -> dict[str, str]:
    """Keep ~max_words entries without dropping OEWN/Webster must-keeps.

    Earlier trim ranked by priority first, so all slots filled with Wiktionary
    (100) and literary OEWN/Webster words like "pendulous" vanished — even when
    those lemmas were also in OEWN (still stored as priority 100 from Wiktionary).
    """
    if max_words <= 0 or len(entries) <= max_words:
        return entries

    def is_formish(defn: str) -> bool:
        first = defn.split("\n", 1)[0]
        return bool(_FORM_OF_RE.search(first) or _FORM_OF_BASE_RE.search(first[:80]))

    # Always keep WordNet + Webster lemmas (even if Wiktionary already defined them).
    must_keep: set[str] = set()
    if core_lemmas:
        for key in core_lemmas:
            if key in entries:
                must_keep.add(key)
    for key, p in priorities.items():
        if p in (50, 10) and key in entries:  # oewn-only / webster-only fills
            must_keep.add(key)

    remaining_budget = max(0, max_words - len(must_keep))
    candidates: list[tuple[int, int, int, str]] = []
    for key, defn in entries.items():
        if key in must_keep:
            continue
        p = priorities.get(key, 0)
        # Prefer non-form-of Wiktionary heads; then richer defs.
        formish = 1 if is_formish(defn) else 0
        candidates.append((0 if formish else 1, p, min(len(defn), 800), key))
    candidates.sort(reverse=True)

    keep_keys = set(must_keep)
    for *_, key in candidates:
        if len(keep_keys) >= max_words:
            break
        keep_keys.add(key)

    # Pull in form-of bases referenced by kept entries.
    for key in list(keep_keys):
        defn = entries.get(key, "")
        m = _FORM_OF_BASE_RE.search(defn)
        if m:
            base = normalize_key(m.group(1))
            if base in entries:
                keep_keys.add(base)

    trimmed = {k: entries[k] for k in keep_keys if k in entries}
    if len(trimmed) > max_words:
        # Prefer must_keep, then non-formish, then higher priority.
        scored2: list[tuple[int, int, int, str]] = []
        for k, defn in trimmed.items():
            must = 1 if k in must_keep else 0
            scored2.append((must, 0 if is_formish(defn) else 1, priorities.get(k, 0), k))
        scored2.sort(reverse=True)
        trimmed = {k: trimmed[k] for *_, k in scored2[:max_words]}

    print(
        f"trim: {len(entries)} -> {len(trimmed)} (max_words={max_words}, must_keep={len(must_keep)})"
    )
    for k in list(priorities.keys()):
        if k not in trimmed:
            del priorities[k]
    return trimmed


def load_oewn(oewn_dir: Path, priorities: dict[str, int], entries: dict[str, str]) -> set[str]:
    """Open English WordNet: gap-fill defs. Returns all lemma keys (must-keep set)."""
    lemmas: set[str] = set()
    if not oewn_dir.is_dir():
        print(f"warn: OEWN dir missing: {oewn_dir}", file=sys.stderr)
        return lemmas
    print(f"oewn/wordnet: loading {oewn_dir} ...", flush=True)
    files = sorted(
        p for p in oewn_dir.glob("*.json") if not p.name.startswith("entries") and p.name != "frames.json"
    )
    lemma_senses: dict[str, list[str]] = {}
    for path in files:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001
            print(f"warn: skip {path.name}: {exc}", file=sys.stderr)
            continue
        if not isinstance(data, dict):
            continue
        for _sid, ss in data.items():
            if not isinstance(ss, dict):
                continue
            defs = ss.get("definition") or []
            if not defs:
                continue
            defn = str(defs[0]).strip()
            members = [str(m) for m in (ss.get("members") or [])]
            single = [m for m in members if "_" not in m and " " not in m]
            for m in single:
                key = normalize_key(m)
                if len(key) < 2:
                    continue
                lemmas.add(key)
                bucket = lemma_senses.setdefault(key, [])
                if defn not in bucket and len(bucket) < 3:
                    bucket.append(defn)

    added = 0
    for key, senses in lemma_senses.items():
        defn = clean_def("; ".join(senses))
        if add_entry(entries, key, defn, priority=50, priorities=priorities):
            added += 1
    print(f"oewn/wordnet: lemmas={len(lemmas)} newly_filled={added} total={len(entries)}")
    return lemmas


def load_webster(path: Path, priorities: dict[str, int], entries: dict[str, str]) -> set[str]:
    """Webster gap-fill. Returns all lemma keys seen (must-keep set)."""
    lemmas: set[str] = set()
    if not path.is_file():
        print(f"warn: Webster JSON missing: {path}", file=sys.stderr)
        return lemmas
    print(f"webster: loading {path.name} (gap-fill only) ...", flush=True)
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        return lemmas
    added = 0
    for word, defn in data.items():
        if isinstance(defn, list):
            defn = "; ".join(str(x) for x in defn if x)
        key = normalize_key(str(word))
        if len(key) >= 2:
            lemmas.add(key)
        # Low priority — only fills holes.
        if add_entry(entries, str(word), str(defn), priority=10, priorities=priorities):
            added += 1
    print(f"webster: lemmas={len(lemmas)} newly_filled={added} total={len(entries)}")
    return lemmas


def load_tsv(path: Path, priorities: dict[str, int], entries: dict[str, str], priority: int = 110) -> None:
    n = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "\t" not in line:
            continue
        word, defn = line.split("\t", 1)
        if add_entry(entries, word, defn, priority=priority, priorities=priorities):
            n += 1
    print(f"tsv: merges={n} total={len(entries)}")


def build(entries: dict[str, str]) -> bytes:
    items = sorted(entries.items(), key=lambda kv: kv[0])
    data = bytearray()
    index = bytearray()
    for key, defn in items:
        def_bytes = defn.encode("utf-8")
        if len(def_bytes) > MAX_DEF_BYTES:
            def_bytes = def_bytes[:MAX_DEF_BYTES]
        offset = len(data)
        data.extend(def_bytes)
        key_bytes = key.encode("ascii", errors="ignore")[: KEY_LEN - 1]
        key_field = key_bytes + b"\0" * (KEY_LEN - len(key_bytes))
        index.extend(key_field)
        index.extend(struct.pack("<II", offset, len(def_bytes)))

    header = MAGIC + struct.pack("<III", len(items), 16, 16 + len(index))
    assert len(header) == 16
    return header + index + data


def resolve_kaikki_path(explicit: Path | None) -> Path | None:
    if explicit and explicit.is_file():
        return explicit
    for cand in (DEFAULT_KAIKKI_GZ, DEFAULT_KAIKKI_JSONL):
        if cand.is_file():
            return cand
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("-i", "--input", type=Path, help="Optional high-priority TSV merge")
    ap.add_argument("--kaikki", type=Path, default=None, help="Path to kaikki English .jsonl or .jsonl.gz")
    ap.add_argument("--oewn", type=Path, default=DEFAULT_OEWN)
    ap.add_argument("--webster", type=Path, default=DEFAULT_WEBSTER)
    ap.add_argument("--no-kaikki", action="store_true")
    ap.add_argument("--no-oewn", action="store_true")
    ap.add_argument("--no-webster", action="store_true")
    ap.add_argument("--min-words", type=int, default=0, help="Fail if fewer than N words")
    ap.add_argument(
        "--max-words",
        type=int,
        default=DEFAULT_MAX_WORDS,
        help=f"Cap pack size for a compact reader dictionary (default {DEFAULT_MAX_WORDS}; 0 = no cap)",
    )
    ap.add_argument(
        "--max-senses",
        type=int,
        default=DEFAULT_MAX_SENSES,
        help=f"Max glosses per POS block (default {DEFAULT_MAX_SENSES})",
    )
    ap.add_argument(
        "--max-forms",
        type=int,
        default=DEFAULT_MAX_FORMS_PER_HEAD,
        help=f"Max inflection forms stored per headword (default {DEFAULT_MAX_FORMS_PER_HEAD})",
    )
    ap.add_argument(
        "--full",
        action="store_true",
        help="Disable reader caps (no max-words, more senses/forms)",
    )
    args = ap.parse_args()

    if args.full:
        args.max_words = 0
        args.max_senses = 3
        args.max_forms = 24

    entries: dict[str, str] = {}
    priorities: dict[str, int] = {}
    # OEWN + Webster lemmas must survive max-words trim even when Wiktionary already
    # defined them (priority 100). Without this, literary OEWN words like "pendulous"
    # were dropped once Wiktionary alone filled the 280k budget with rarer heads/forms.
    core_lemmas: set[str] = set()

    if not args.no_kaikki:
        kpath = resolve_kaikki_path(args.kaikki)
        if kpath:
            load_kaikki(
                kpath,
                priorities,
                entries,
                max_senses=max(1, args.max_senses),
                max_forms_per_head=max(0, args.max_forms),
            )
        else:
            print(
                "warn: no kaikki English dump found.\n"
                "  Download:\n"
                "  https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl.gz\n"
                f"  to {DEFAULT_KAIKKI_GZ}",
                file=sys.stderr,
            )

    if not args.no_oewn:
        core_lemmas |= load_oewn(args.oewn, priorities, entries)

    if not args.no_webster:
        core_lemmas |= load_webster(args.webster, priorities, entries)

    if args.input:
        load_tsv(args.input, priorities, entries, priority=110)

    if not entries:
        print("error: no entries built", file=sys.stderr)
        return 1

    # Expand leftover "plural of X" / "participle of X" glosses using base entries.
    expand_form_of_entries(entries)

    # Seed / repair ultra-common words that sources sometimes omit.
    for key, defn in CORE_FUNCTION_WORDS.items():
        if key not in entries or len(entries.get(key, "")) < 12:
            entries[key] = defn
            priorities[key] = 200
            core_lemmas.add(key)
            print(f"core seed: {key}")

    if args.max_words > 0:
        entries = trim_to_max_words(
            entries, priorities, args.max_words, core_lemmas=core_lemmas or None
        )

    if args.min_words and len(entries) < args.min_words:
        print(f"error: only {len(entries)} words, need >= {args.min_words}", file=sys.stderr)
        return 1

    # Source mix stats
    from collections import Counter

    c = Counter(priorities.values())
    print(
        "source mix (priority counts):",
        {100: "wiktionary-head", 90: "wiktionary-form", 50: "oewn", 10: "webster"}.items(),
    )
    print("priority histogram:", dict(sorted(c.items(), reverse=True)))

    blob = build(entries)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f"wrote {args.output} ({len(blob)} bytes, {len(entries)} words)")
    print("Copy to SD: /.crosspoint/dict/en.cxdict")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
