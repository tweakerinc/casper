#!/usr/bin/env python3
"""Probe Calibre library for EPUB layout features that stress a constrained e-reader."""
from __future__ import annotations

import random
import re
import zipfile
from collections import Counter
from pathlib import Path

LIB = Path(r"e:\Calibre eBooks")


def pick_band(epubs: list[Path], lo: int, hi: int, n: int = 4) -> list[Path]:
    band = [e for e in epubs if lo <= e.stat().st_size < hi]
    random.seed(42)
    return random.sample(band, min(n, len(band))) if band else []


def probe(path: Path) -> dict:
    try:
        z = zipfile.ZipFile(path)
    except Exception as ex:  # noqa: BLE001
        return {"book": path.name[:50], "err": str(ex)[:60]}

    names = z.namelist()
    html = [n for n in names if n.lower().endswith((".xhtml", ".html", ".htm"))]
    css = [n for n in names if n.lower().endswith(".css")]
    imgs = [n for n in names if n.lower().endswith((".jpg", ".jpeg", ".png", ".gif", ".svg", ".webp"))]
    text = ""
    for n in css[:16]:
        try:
            text += z.read(n).decode("utf-8", "ignore")
        except Exception:
            pass
    for n in html[:10]:
        try:
            text += z.read(n).decode("utf-8", "ignore")[:250_000]
        except Exception:
            pass

    def has(pat: str) -> bool:
        return bool(re.search(pat, text, re.I))

    max_html = 0
    for n in html:
        try:
            max_html = max(max_html, z.getinfo(n).file_size)
        except Exception:
            pass

    return {
        "book": path.name[:48],
        "MB": round(path.stat().st_size / 1e6, 2),
        "html": len(html),
        "css": len(css),
        "img": len(imgs),
        "float": has(r"float\s*:"),
        "table": has(r"<table|display\s*:\s*table"),
        "dropcap": has(r"drop.?cap|::first-letter|first-letter"),
        "smallcaps": has(r"small-caps|font-variant"),
        "fontsize": has(r"font-size\s*:"),
        "lineheight": has(r"line-height\s*:"),
        "strikethrough": has(r"line-through"),
        "columns": has(r"column-count|columns\s*:"),
        "position": has(r"position\s*:\s*(absolute|fixed|relative)"),
        "gif": any(n.lower().endswith(".gif") for n in imgs),
        "svg": any(n.lower().endswith(".svg") for n in imgs),
        "maxHtmlKB": round(max_html / 1024, 1),
    }


def main() -> None:
    epubs = list(LIB.rglob("*.epub"))
    print(f"epubs={len(epubs)}")
    if not epubs:
        return

    sizes = sorted(e.stat().st_size for e in epubs)
    print(
        "size_MB p50={:.2f} p90={:.2f} max={:.2f}".format(
            sizes[len(sizes) // 2] / 1e6,
            sizes[int(len(sizes) * 0.9)] / 1e6,
            sizes[-1] / 1e6,
        )
    )

    samples: list[Path] = []
    samples += pick_band(epubs, 0, 1_000_000, 5)
    samples += pick_band(epubs, 1_000_000, 5_000_000, 6)
    samples += pick_band(epubs, 5_000_000, 80_000_000, 6)
    samples += sorted(epubs, key=lambda p: p.stat().st_size, reverse=True)[:4]

    seen: set[Path] = set()
    uniq: list[Path] = []
    for e in samples:
        if e not in seen:
            seen.add(e)
            uniq.append(e)

    rows = [probe(e) for e in uniq]
    c: Counter[str] = Counter()
    for r in rows:
        if "err" in r:
            continue
        for k in (
            "float",
            "table",
            "dropcap",
            "smallcaps",
            "fontsize",
            "lineheight",
            "strikethrough",
            "columns",
            "position",
            "gif",
            "svg",
        ):
            if r.get(k):
                c[k] += 1

    ok = [r for r in rows if "err" not in r]
    print(f"SAMPLES={len(ok)}")
    print("FEATURE_HITS", dict(c))
    print("---")
    for r in sorted(ok, key=lambda x: -x["MB"]):
        flags = ",".join(
            k
            for k in (
                "float",
                "table",
                "dropcap",
                "smallcaps",
                "fontsize",
                "lineheight",
                "strikethrough",
                "columns",
                "position",
                "gif",
                "svg",
            )
            if r.get(k)
        )
        print(
            f"{r['MB']:5.2f}MB html={r['html']:3d} css={r['css']:2d} img={r['img']:3d} "
            f"maxH={r['maxHtmlKB']:7.1f}KB | {flags:50s} | {r['book']}"
        )


if __name__ == "__main__":
    main()
