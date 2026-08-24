#!/usr/bin/env python3
"""Build a Nadsat StarDict pack for CrossPoint / XTEINK from This Chick Reads.

Primary source (plain-English origins the reader preferred):
  https://thischickreads.com/nadsat-language-glossary/

Format (CrossPoint docs/dictionary.md):
  /dictionaries/Nadsat/nadsat.{ifo,idx,dict}
  sametypesequence=m, 32-bit offsets, plain .dict
  Latin script only (no Cyrillic — stock device fonts are Latin)

Definition shape (no repeated book banner):
  <gloss>

  Origin: <plain English where it came from>

Meta headword "nadsat" alone carries:
  Fictional teen slang invented by Anthony Burgess for A Clockwork Orange.

Usage:
  python scripts/build_nadsat_dict.py
  python scripts/build_nadsat_dict.py --html scripts/data/nadsat_thischickreads.html
  python scripts/build_nadsat_dict.py --fetch
"""
from __future__ import annotations

import argparse
import html as html_lib
import re
import struct
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HTML = Path(__file__).resolve().parent / "data" / "nadsat_thischickreads.html"
DEFAULT_TSV = Path(__file__).resolve().parent / "data" / "nadsat.tsv"
DEFAULT_OUT = ROOT / "dist" / "dictionaries"
SOURCE_URL = "https://thischickreads.com/nadsat-language-glossary/"

_NON_LATIN = re.compile(
    r"[\u0400-\u04FF\u0500-\u052F\u2DE0-\u2DFF\uA640-\uA69F"
    r"\u0370-\u03FF\u1F00-\u1FFF]+"
)
_WS = re.compile(r"\s+")
# First sentence(s) that explain origin; drop long literary digressions.
_ORIGIN_START = re.compile(
    r"(?is)^("
    r"(?:borrowed|taken|derived|straight|from|short(?:ened)?|a |an |this |"
    r"likely|possibly|seems|combining|playful|phonetic|onomatop|"
    r"english|scots|russian|french|italian|german|cockney|blend|"
    r"juvenile|diminutive|umbrella|key term|not exclusively).{10,400}?"
    r"(?:[.!?]|$)"
    r")"
)


def fetch_html(dest: Path) -> Path:
    req = urllib.request.Request(
        SOURCE_URL,
        headers={"User-Agent": "CrossPointDictBuilder/1.0 (offline glossary pack; +https://github.com/)"},
    )
    with urllib.request.urlopen(req, timeout=45) as resp:
        data = resp.read()
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    print(f"Fetched {len(data)} bytes → {dest}")
    return dest


def strip_tags(fragment: str) -> str:
    t = re.sub(r"<br\s*/?>", "\n", fragment, flags=re.I)
    t = re.sub(r"</p>", "\n", t, flags=re.I)
    t = re.sub(r"<[^>]+>", " ", t)
    t = html_lib.unescape(t)
    t = _WS.sub(" ", t).strip()
    return t


def latin_only(s: str) -> str:
    if not s:
        return s
    # Normalize curly quotes before stripping scripts
    s = s.replace("\u201c", '"').replace("\u201d", '"')
    s = s.replace("\u2018", "'").replace("\u2019", "'")
    s = s.replace("\u2014", "-").replace("\u2013", "-")
    s = s.replace("\u00a0", " ")
    s = _NON_LATIN.sub("", s)
    # "" (babushka) left after removing Cyrillic inside quotes → babushka
    s = re.sub(r'""\s*\(([^)]+)\)', r"\1", s)
    s = re.sub(r"''\s*\(([^)]+)\)", r"\1", s)
    s = re.sub(r'"\s*"', "", s)
    s = re.sub(r"'\s*'", "", s)
    s = re.sub(r"\(\s*\)", "", s)
    s = re.sub(r"\[\s*\]", "", s)
    # "Russian word  (babushka)" or "Russian  babushka"
    s = re.sub(r"\bword\s+\(", "word (", s)
    s = re.sub(r"\s{2,}", " ", s)
    s = re.sub(r"\s+([,.;:])", r"\1", s)
    s = re.sub(r"\(\s+", "(", s)
    s = re.sub(r"\s+\)", ")", s)
    s = s.strip()
    if s.startswith("(") and s.endswith(")") and s.count("(") == 1:
        s = s[1:-1].strip()
    if s.count(")") > s.count("("):
        s = s.replace(")", "", s.count(")") - s.count("("))
    if s.count("(") > s.count(")"):
        s = s.replace("(", "", s.count("(") - s.count(")"))
    return s.strip(" ;,|-")


def normalize_key(word: str) -> str:
    w = word.strip().lower()
    if not w:
        return ""
    out: list[str] = []
    for ch in w:
        o = ord(ch)
        if 97 <= o <= 122 or 48 <= o <= 57 or ch in "-' /":
            out.append(ch if ch != "/" else " ")
        elif o > 127 and not ("\u0400" <= ch <= "\u04ff"):
            out.append(ch)
    w = _WS.sub(" ", "".join(out)).strip(" -'")
    return w


def expand_headwords(cell: str) -> list[str]:
    cell = latin_only(cell)
    cell = re.sub(r"\([^)]*\)", "", cell)
    parts = re.split(r"[,;/]+", cell)
    keys: list[str] = []
    for p in parts:
        # "faggy/fagged" already split by / above into space then... handle slash
        for sub in re.split(r"[/]+", p):
            k = normalize_key(sub)
            if k and k not in ("see", "section"):
                keys.append(k)
    return keys


def extract_gloss_and_heads(h4_text: str) -> tuple[list[str], str]:
    """'Droog: Friend.' → heads=['droog'], gloss='Friend'."""
    t = latin_only(h4_text).strip().rstrip(".")
    if ":" in t:
        left, right = t.split(":", 1)
        heads = expand_headwords(left)
        gloss = right.strip().rstrip(".")
        return heads, gloss
    # No colon — whole thing is headword
    return expand_headwords(t), ""


def origin_from_body(body: str, gloss: str) -> str:
    """Keep plain-English origin; drop long essay tail. Latin only."""
    body = latin_only(body)
    if not body:
        return ""
    # Prefer first 1–2 sentences that explain etymology / formation
    sentences = re.split(r"(?<=[.!?])\s+", body)
    keep: list[str] = []
    total = 0
    for i, sent in enumerate(sentences):
        s = sent.strip()
        if not s:
            continue
        # Skip pure literary waffle after we already have an origin sentence
        if keep and i >= 2:
            break
        if keep and re.match(
            r"(?i)^(it'?s |this is |alex |in the novel|in nadsat,|burgess'?s use|"
            r"the term feels|its clipped|a key theme)",
            s,
        ):
            break
        keep.append(s)
        total += len(s)
        if total >= 280:
            break
        # One solid origin sentence is enough when it starts with From/Borrowed/...
        if i == 0 and re.match(
            r"(?i)^(borrowed|taken|derived|from |straight from|short|"
            r"a phonetic|a blend|english slang|scots|likely|possibly|"
            r"onomatop|combining|this term is an|this term comes|"
            r"this playful|this juvenile|this fascinating|this variation|"
            r"this word |short for|shortened)",
            s,
        ):
            # allow a short second sentence if still short
            if total >= 120:
                break
    origin = " ".join(keep).strip()
    if len(origin) > 360:
        origin = origin[:357].rsplit(" ", 1)[0] + "..."
    # Don't repeat the gloss if the body starts with it
    g = gloss.strip().rstrip(".").lower()
    if g and origin.lower().startswith(g + "."):
        origin = origin[len(g) + 1 :].strip()
    elif g and origin.lower().startswith(g + " "):
        # "Friend. Straight from..." already handled; "Friend Straight" rare
        pass
    return origin


def format_def(gloss: str, origin: str) -> str:
    gloss = latin_only(gloss).strip().rstrip(".")
    origin = latin_only(origin).strip()
    if not gloss:
        gloss = "(no gloss)"
    # Capitalize gloss lightly for display
    if gloss and gloss[0].islower():
        gloss = gloss[0].upper() + gloss[1:]
    lines = [gloss]
    if origin and origin.lower() not in (gloss.lower(),):
        lines.append("")
        # Single Origin: line — no "Nadsat — A Clockwork Orange" banner
        if not origin.lower().startswith("origin:"):
            lines.append(f"Origin: {origin}")
        else:
            lines.append(origin)
    return "\n".join(lines).strip()


def parse_thischickreads_html(html: str) -> dict[str, str]:
    # Drop scripts/styles
    html = re.sub(r"<script[\s\S]*?</script>", "", html, flags=re.I)
    html = re.sub(r"<style[\s\S]*?</style>", "", html, flags=re.I)

    entries: dict[str, str] = {}
    # h4 title + following first paragraph
    pattern = re.compile(
        r"<h4[^>]*>([\s\S]*?)</h4>\s*(?:<p[^>]*>([\s\S]*?)</p>)?",
        re.I,
    )
    for m in pattern.finditer(html):
        h4 = strip_tags(m.group(1))
        body = strip_tags(m.group(2) or "")
        if not h4 or len(h4) > 120:
            continue
        # Skip non-glossary headings
        if h4.lower() in ("table of contents",) or not re.search(r"[a-zA-Z]", h4):
            continue
        heads, gloss = extract_gloss_and_heads(h4)
        if not heads:
            continue
        if not gloss:
            # Try first sentence of body as gloss
            first = body.split(".")[0].strip() if body else ""
            gloss = first[:80] if first else heads[0]
        origin = origin_from_body(body, gloss)
        defn = format_def(gloss, origin)
        for key in heads:
            if key not in entries or len(defn) > len(entries[key]):
                entries[key] = defn
    return entries


def write_tsv(path: Path, entries: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(f"# Nadsat glossary for CrossPoint — source: {SOURCE_URL}\n")
        f.write("# headword\\tdefinition (UTF-8, Latin-only)\n")
        f.write("headword\tdefinition\n")
        for k in sorted(entries.keys(), key=lambda s: s.lower()):
            # single-line for TSV
            defn = entries[k].replace("\t", " ").replace("\n", " | ")
            f.write(f"{k}\t{defn}\n")


def add_meta_and_aliases(entries: dict[str, str]) -> None:
    # Single place for book credit — not repeated on every word.
    entries["nadsat"] = (
        "Teenage slang; the invented argot spoken by Alex and his droogs.\n\n"
        "Fictional language written by Anthony Burgess for the novel "
        "A Clockwork Orange (mostly Russian-influenced English slang, "
        "with Cockney, Romani, and playful coinages)."
    )

    aliases = {
        "droogs": "droog",
        "glazzies": "glazz",
        "glazzballs": "glazz",
        "ptitsas": "ptitsa",
        "vecks": "veck",
        "chellovecks": "chelloveck",
        "devotchkas": "devotchka",
        "baboochkas": "baboochka",
        "millicents": "millicent",
        "gullivers": "gulliver",
        "yarbles": "yarblockos",
        "ultraviolence": "ultra-violence",
        "ultra violence": "ultra-violence",
        "appy-polly-loggies": "appy polly loggies",
        "appy polly loggy": "appy polly loggies",
        "moloko plus": "moloko plus",
        "moloko+": "moloko plus",
        "bratty": "brat",
    }
    for alias, target in aliases.items():
        a = normalize_key(alias)
        t = normalize_key(target)
        if a in entries:
            continue
        if t in entries:
            entries[a] = entries[t]
            continue
        for k, v in list(entries.items()):
            if k == t or k.replace("-", " ") == t.replace("-", " "):
                entries[a] = v
                break


def write_stardict(out_dir: Path, stem: str, bookname: str, entries: dict[str, str], description: str) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    items = sorted(
        ((k, v) for k, v in entries.items() if k and v),
        key=lambda kv: (kv[0].lower(), kv[0]),
    )
    dict_parts: list[bytes] = []
    idx_parts: list[bytes] = []
    offset = 0
    for word, defn in items:
        body = defn.encode("utf-8", "replace")
        if len(body) > 4096:
            body = body[:4096]
        # Safety: never ship Cyrillic in the binary pack
        if any("\u0400" <= c <= "\u04ff" for c in defn):
            body = latin_only(defn).encode("utf-8", "replace")
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
    qidx = out_dir / f"{stem}.qidx"
    if qidx.exists():
        qidx.unlink()
    print(f"Wrote {out_dir}")
    print(f"  words={len(items)}  dict={len(dict_data)} B  idx={len(idx_data)} B")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fetch", action="store_true", help=f"Download {SOURCE_URL}")
    ap.add_argument("--html", type=Path, default=DEFAULT_HTML, help="Saved HTML of the glossary")
    ap.add_argument("--tsv", type=Path, default=DEFAULT_TSV, help="Write derived TSV here")
    ap.add_argument("-o", "--out", type=Path, default=DEFAULT_OUT, help="dictionaries root")
    args = ap.parse_args()

    if args.fetch or not args.html.is_file():
        try:
            fetch_html(args.html)
        except Exception as e:
            if not args.html.is_file():
                print(f"ERROR: could not fetch glossary: {e}", file=sys.stderr)
                return 1
            print(f"WARN: fetch failed ({e}); using cached {args.html}")

    html = args.html.read_text(encoding="utf-8", errors="replace")
    entries = parse_thischickreads_html(html)
    print(f"Parsed {len(entries)} headwords from This Chick Reads HTML")
    if len(entries) < 80:
        print("ERROR: too few entries — HTML structure may have changed", file=sys.stderr)
        return 1

    add_meta_and_aliases(entries)
    write_tsv(args.tsv, entries)
    print(f"Wrote TSV {args.tsv}")

    for k in ("droog", "horrorshow", "moloko", "devotchka", "bezoomny", "nadsat", "britva"):
        print(f"  check {k}: {'ok' if k in entries else 'MISSING'}")

    out_dir = args.out / "Nadsat"
    write_stardict(
        out_dir,
        stem="nadsat",
        bookname="Nadsat",
        entries=entries,
        description=(
            "Nadsat slang glossary for A Clockwork Orange. "
            f"Adapted from {SOURCE_URL} (plain-English origins). "
            "Latin script only for XTEINK/CrossPoint fonts. "
            "Meta entry 'nadsat' credits Anthony Burgess."
        ),
    )

    readme = out_dir / "README.txt"
    readme.write_text(
        "Nadsat dictionary for CrossPoint / XTEINK\n"
        "=====================================\n\n"
        "Install: copy this folder to the SD card:\n\n"
        "  /dictionaries/Nadsat/\n\n"
        "    nadsat.ifo\n"
        "    nadsat.idx\n"
        "    nadsat.dict\n\n"
        "Settings → Reader → Dictionary → enable Nadsat\n"
        "(also enable English for normal English words).\n\n"
        "Definitions: plain English gloss + Origin line.\n"
        "No Cyrillic (stock fonts are Latin-only).\n"
        "Look up 'nadsat' for the language credit:\n"
        "  Fictional language by Anthony Burgess for A Clockwork Orange.\n\n"
        f"Source: {SOURCE_URL}\n"
        "Rebuild: python scripts/build_nadsat_dict.py --fetch\n",
        encoding="utf-8",
    )
    print(f"Wrote {readme}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
