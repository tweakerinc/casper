#!/usr/bin/env python3
"""Render a before/after PNG of the stem-calibration size change for social media.

For each font it composites the pangram from the actual .cpfont glyph bitmaps
(the same advance+bearings compositing generate_preview.py uses) at Small (12)
and Medium (14), BEFORE the size change (nominal render) and AFTER it
(calibrated render), and stacks them into one tall PNG with the 2-bit pixels
scaled up so the gray stem fringe is visible.

  before  = nominal render (still gap-fixed)   ~/BulkDocuments/crosspoint-fonts
  after S = Small calibrated                   ~/BulkDocuments/crosspoint-fonts-calibrated
  after M = Small+Medium calibrated            ~/BulkDocuments/crosspoint-fonts-calibrated-medium

Usage:
    python3 social_preview.py                       # all fonts, default packs
    python3 social_preview.py --fonts Lexica,IBMPlexMono --scale 6
    python3 social_preview.py --text "Hamburgevons" --out post.png
"""
import os, glob, argparse
from PIL import Image, ImageDraw, ImageFont
from cpfont_engine import CpFont

PANGRAM = '"The quick brown fox jumps over the lazy dog."'

# 2-bit e-ink levels -> grayscale on white (matches the device's 4/8/12 ramp feel)
INK = {0: 255, 1: 190, 2: 105, 3: 0}


def compose(path, text):
    """Composite `text` from a .cpfont (style 0). Returns (W, H, grid) of levels."""
    cf = CpFont(path)
    st = cf.styles[0]
    asc, desc = st["ascender"], st["descender"]
    lineH = asc - desc
    pad = 2
    glyphs, pen, maxx = [], 0, 0
    for ch in text:
        d = cf.get(0, ord(ch))
        if d is None:
            pen += lineH // 2
            continue
        x0 = pen + d["left"]
        glyphs.append((x0, asc - d["top"], d))
        maxx = max(maxx, x0 + d["w"])
        pen += round(d["adv"] / 16)
    W = max(pen, maxx) + pad
    H = lineH + pad * 2
    grid = [[0] * W for _ in range(H)]
    for x0, y0, d in glyphs:
        for yy in range(d["h"]):
            gy = y0 + yy + pad
            if 0 <= gy < H:
                row = d["levels"][yy]
                for xx in range(d["w"]):
                    gx = x0 + xx
                    if 0 <= gx < W and row[xx] and row[xx] > grid[gy][gx]:
                        grid[gy][gx] = row[xx]
    return W, H, grid


def strip_image(grid, W, H, scale):
    """Render a level-grid to a scaled (pixelated) grayscale PIL image."""
    img = Image.new("L", (W, H), 255)
    px = img.load()
    for y in range(H):
        row = grid[y]
        for x in range(W):
            v = row[x]
            if v:
                px[x, y] = INK[v]
    return img.resize((W * scale, H * scale), Image.NEAREST)


def _font(size):
    try:
        return ImageFont.load_default(size=size)   # Pillow >= 10
    except TypeError:
        return ImageFont.load_default()


GREEN, GREY, DARK = (0, 150, 110), (120, 120, 120), (30, 30, 30)

_MEASURE = ImageDraw.Draw(Image.new("RGB", (1, 1)))


def _th(text, font):
    """True pixel height of `text` in `font` (incl. descenders)."""
    b = _MEASURE.textbbox((0, 0), text, font=font, anchor="lt")
    return b[3] - b[1]


def render_panel(fonts, title, before, after, changed):
    """One self-contained before -> after panel (horizontal, arrow between).

    `before`/`after` are (tag, img). Returns a white RGB PIL image."""
    fL, fS = fonts
    btag, bimg = before
    atag, aimg = after
    PAD, ARROW, GAP_T, GAP_B = 20, 104, 12, 8
    title_h = _th(title, fL)
    tag_h = max(_th(btag, fS), _th(atag, fS))
    band = max(bimg.height, aimg.height)
    head = PAD + title_h + GAP_T
    arrow_col = GREEN if changed else GREY

    W = PAD + bimg.width + ARROW + aimg.width + PAD
    H = head + band + GAP_B + tag_h + PAD
    img = Image.new("RGB", (W, H), (255, 255, 255))
    dr = ImageDraw.Draw(img)

    dr.text((PAD, PAD), title, font=fL, fill=DARK, anchor="lt")

    bx, ax = PAD, PAD + bimg.width + ARROW
    img.paste(bimg, (bx, head + (band - bimg.height) // 2))
    img.paste(aimg, (ax, head + (band - aimg.height) // 2))

    # arrow across the gap, vertically centred on the sample band
    ymid = head + band // 2
    x0, x1 = bx + bimg.width + 14, ax - 14
    dr.line((x0, ymid, x1 - 6, ymid), fill=arrow_col, width=5)
    dr.polygon([(x1, ymid), (x1 - 15, ymid - 10), (x1 - 15, ymid + 10)], fill=arrow_col)
    if changed:
        lbl = "calibrated"
        lw = dr.textlength(lbl, font=fS)
        dr.text(((x0 + x1) / 2 - lw / 2, ymid - 24), lbl, font=fS, fill=GREEN, anchor="lt")

    ty = head + band + GAP_B
    dr.text((bx, ty), btag, font=fS, fill=GREY, anchor="lt")
    dr.text((ax, ty), atag, font=fS, fill=arrow_col, anchor="lt")
    return img


def main():
    ap = argparse.ArgumentParser()
    home = os.path.expanduser("~/BulkDocuments")
    ap.add_argument("--before", default=f"{home}/crosspoint-fonts",
                    help="nominal pack (before the size change)")
    ap.add_argument("--after-small", default=f"{home}/crosspoint-fonts-calibrated")
    ap.add_argument("--after-medium", default=f"{home}/crosspoint-fonts-calibrated-medium")
    ap.add_argument("--text", default=PANGRAM)
    ap.add_argument("--scale", type=int, default=5, help="integer pixel zoom")
    ap.add_argument("--fonts", default="", help="comma list substring filter (default all)")
    ap.add_argument("--select", default="",
                    help="per-(font,size) showcase, ordered, e.g. "
                         "'Lexica:Small,Lexica:Medium,GentiumBookPlus:Small'. "
                         "Family is substring-matched; omit ':Size' for both sizes.")
    ap.add_argument("--only-changed", action="store_true",
                    help="skip a size when before == after (no calibration applied)")
    ap.add_argument("--per-font", action="store_true",
                    help="also write one PNG per (font,size) panel next to --out")
    ap.add_argument("-o", "--out", default="social_preview.png")
    a = ap.parse_args()

    # sizes: (label, nominal-size, after-pack)
    SIZES = [("Small", "12", a.after_small), ("Medium", "14", a.after_medium)]

    allfams = sorted(os.path.basename(d) for d in glob.glob(a.before + "/*") if os.path.isdir(d))

    # targets: ordered [(family, set-of-size-labels or None=both)]
    if a.select:
        sel = {}  # family-substring (lower) -> set of labels (empty = both)
        for entry in a.select.split(","):
            entry = entry.strip()
            if not entry:
                continue
            key, _, sz = entry.partition(":")
            s = sel.setdefault(key.strip().lower(), set())
            if sz.strip():
                s.add(sz.strip().capitalize())
        targets = []
        for key, labels in sel.items():
            match = next((f for f in allfams if key in f.lower()), None)
            if match is None:
                print(f"  ! no font matches '{key}'")
                continue
            targets.append((match, labels or None))
    else:
        fams = allfams
        if a.fonts:
            wants = [w.strip().lower() for w in a.fonts.split(",") if w.strip()]
            fams = [f for f in fams if any(w in f.lower() for w in wants)]
        targets = [(f, None) for f in fams]

    sc = a.scale
    fonts = (_font(30), _font(17))     # panel title, tag/label
    PANEL_GAP, MARGIN = 16, 24

    # ---- build one before->after panel per (font, size) ----
    panels = []        # (slug, image)
    for fam, want_labels in targets:
        for label, size, after_dir in SIZES:
            if want_labels is not None and label not in want_labels:
                continue
            bpath = f"{a.before}/{fam}/{fam}_{size}.cpfont"
            apath = f"{after_dir}/{fam}/{fam}_{size}.cpfont"
            if not (os.path.exists(bpath) and os.path.exists(apath)):
                continue
            try:
                bw, bh, bg = compose(bpath, a.text)
                aw, ah, ag = compose(apath, a.text)
            except Exception as e:
                print(f"  skip {fam} {size}: {e}")
                continue
            changed = open(bpath, "rb").read() != open(apath, "rb").read()
            if a.only_changed and not changed:
                continue
            before = (f"before  -  nominal {size}pt  -  {bw}x{bh}px",
                      strip_image(bg, bw, bh, sc))
            after = (f"after  -  calibrated  -  {aw}x{ah}px" + ("" if changed else "  (unchanged)"),
                     strip_image(ag, aw, ah, sc))
            title = f"{fam}  -  {label}"
            panel = render_panel(fonts, title, before, after, changed)
            panels.append((f"{fam}_{label}", panel))
            print(f"  {fam} {label}: {bw}x{bh} -> {aw}x{ah}"
                  f"{'' if changed else '  (unchanged)'}")

    if not panels:
        print("No fonts matched / no built packs found.")
        return

    # ---- per-font files ----
    stem, ext = os.path.splitext(a.out)
    if a.per_font:
        for slug, panel in panels:
            p = f"{stem}_{slug}{ext}"
            panel.save(p)
            print(f"  wrote {p}  ({panel.width}x{panel.height})")

    # ---- combined sheet: panels stacked, left-aligned ----
    head = _font(30)
    htxt = "CrossPoint stem calibration  -  before / after the size change"
    head_h = _th(htxt, head)
    content_w = max(p.width for _, p in panels)
    y0 = MARGIN + head_h + 20
    W = max(content_w, 600) + MARGIN * 2
    H = y0 + sum(p.height for _, p in panels) + PANEL_GAP * (len(panels) - 1) + MARGIN
    canvas = Image.new("RGB", (W, H), (255, 255, 255))
    ImageDraw.Draw(canvas).text((MARGIN, MARGIN), htxt, font=head, fill=DARK, anchor="lt")
    y = y0
    for _, panel in panels:
        canvas.paste(panel, (MARGIN, y))
        y += panel.height + PANEL_GAP
    canvas.save(a.out)
    print(f"\nwrote {a.out}  ({W}x{H}, {len(panels)} panels)")


if __name__ == "__main__":
    main()
