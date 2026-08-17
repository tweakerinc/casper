#!/usr/bin/env python3
"""Analyze stem-width consistency in CrossPoint .cpfont (v4) files.

The X4's low PPI magnifies sub-pixel stem quantization: a vertical stem that
FreeType rasterizes at ~1.5px renders as one solid (level-3) column plus a
drifting gray (level-1/2) column. That drift is perceived as "stem width
varies randomly". This tool decodes the 2-bit glyph bitmaps directly from the
.cpfont binary and scores each font for that artifact.

Metrics (per family, style=regular, averaged over a curated straight-stem set):
  stemCoV   coefficient of variation of effective ink-width across interior
            scanlines of straight-stem glyphs. THE stem-width-varies score.
            Higher = stems wobble more. This is what your eye dislikes.
  grayFrac  fraction of total ink that sits in partial levels (1 or 2) rather
            than solid black (level 3). Higher = fuzzier/greyer edges.
No FreeType needed -- reads only the bitmaps already on the card.
"""
import struct, sys, os, glob, math

HEADER_FMT = "<8sHHB19s"          # magic, version, flags, styleCount, reserved
TOC_FMT    = "<B3xIIBhhHHBBBI4x"  # see fontconvert_sdcard.py
GLYPH_FMT  = "<BBHhhH2xI"         # width,height,advX,left,top,dataLen,(pad),dataOff

# Full-height LEFT-vertical-stem letters. We measure only the leftmost ink run
# in a mid-band, which is the bare stem for all of these (bowls/arches/diagonals
# attach above or to the right). 'i'/'j' excluded: their flared top terminals are
# a design feature, not aliasing, and pollute the signal.
PROBE_CPS = [ord(c) for c in "lhnmru"]   # leftmost run == bare stem for all of these

def decode_glyph_levels(data, width, height):
    """Return height x width list of 2-bit levels (0..3) from packed stream."""
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            p = y * width + x
            b = data[p >> 2]
            shift = (3 - (p & 3)) * 2
            row.append((b >> shift) & 3)
        rows.append(row)
    return rows

def parse_cpfont(path):
    with open(path, "rb") as f:
        blob = f.read()
    magic, ver, flags, style_count, _ = struct.unpack_from(HEADER_FMT, blob, 0)
    if magic != b"CPFONT\x00\x00":
        raise ValueError(f"bad magic in {path}")
    styles = {}
    toc_off = 32
    for i in range(style_count):
        (sid, ic, gc, advY, asc, desc, kl, kr, klc, krc, lig, doff) = \
            struct.unpack_from(TOC_FMT, blob, toc_off + i * 32)
        # section sizes within this style
        intervals_sz = ic * 12
        glyphs_sz    = gc * 16
        kl_sz        = kl * 3
        kr_sz        = kr * 3
        km_sz        = klc * krc
        lig_sz       = lig * 8
        bm_start = doff + intervals_sz + glyphs_sz + kl_sz + kr_sz + km_sz + lig_sz
        # parse intervals -> cp lookup
        intervals = []
        for j in range(ic):
            s, e, o = struct.unpack_from("<III", blob, doff + j * 12)
            intervals.append((s, e, o))
        # parse glyphs
        glyphs_off = doff + intervals_sz
        def glyph_index_for_cp(cp):
            for s, e, o in intervals:
                if s <= cp <= e:
                    return o + (cp - s)
            return None
        styles[sid] = dict(ver=ver, gc=gc, glyphs_off=glyphs_off, bm_start=bm_start,
                           blob=blob, gidx=glyph_index_for_cp)
    return styles

def get_glyph(style, cp):
    gi = style["gidx"](cp)
    if gi is None or gi >= style["gc"]:
        return None
    w, h, advX, left, top, dlen, doff = struct.unpack_from(
        GLYPH_FMT, style["blob"], style["glyphs_off"] + gi * 16)
    if w == 0 or h == 0:
        return None
    data = style["blob"][style["bm_start"] + doff: style["bm_start"] + doff + dlen]
    return w, h, decode_glyph_levels(data, w, h)

def stem_metrics(levels, w, h):
    """Measure the LEFT vertical stem of a glyph over a mid-band.

    Mid-band 0.45..0.78 avoids top terminals/arches and bottom feet, where the
    leftmost ink run is the bare stem. Per qualifying row record:
      coverage = sum(level/3) over the leftmost run   -> effective stem width
      graypx   = run pixels at level 1 or 2 (not 3)   -> gray-edge load
    Return (medianCoverage, medianSolidWidth, grayFracOfStemInk, nRows).
    The aggregator turns per-glyph medians into the metrics that matter:
    inter-glyph width spread and overall gray-edge load.
    """
    y0 = int(h * 0.45)
    y1 = max(y0 + 1, int(h * 0.78))
    covs, solids = [], []
    graypx = totpx = 0
    for y in range(y0, y1):
        # leftmost contiguous ink run
        a = None
        for x in range(w):
            if levels[y][x] > 0:
                a = x; break
        if a is None:
            continue
        b = a
        for x in range(a, w):
            if levels[y][x] > 0: b = x
            else: break
        cov = 0.0; sol = 0
        for x in range(a, b + 1):
            lv = levels[y][x]
            cov += lv / 3.0
            totpx += 1
            if lv == 3: sol += 1
            else:       graypx += 1
        if cov < 0.4:
            continue
        covs.append(cov); solids.append(sol)
    if len(covs) < 3:
        return None
    covs.sort(); solids.sort()
    med_cov = covs[len(covs) // 2]
    med_sol = solids[len(solids) // 2]
    gf = graypx / totpx if totpx else 0.0
    return med_cov, med_sol, gf, len(covs)

def analyze_file(path, style_id=0):
    styles = parse_cpfont(path)
    if style_id not in styles:
        return None
    st = styles[style_id]
    covs, sols, gfs = [], [], []
    for cp in PROBE_CPS:
        g = get_glyph(st, cp)
        if not g:
            continue
        w, h, levels = g
        m = stem_metrics(levels, w, h)
        if m:
            covs.append(m[0]); sols.append(m[1]); gfs.append(m[2])
    if len(covs) < 3:
        return None
    mean = sum(covs) / len(covs)
    spread = math.sqrt(sum((c - mean) ** 2 for c in covs) / len(covs))
    return dict(n=len(covs),
                widthSpread=spread,                 # inter-glyph stem-width stdev (px)
                solidSet=sorted(set(sols)),         # distinct solid-core widths
                grayLoad=sum(gfs) / len(gfs),       # mean gray-edge fraction
                meanCov=mean)

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/BulkDocuments/crosspoint-backup/.fonts")
    size = sys.argv[2] if len(sys.argv) > 2 else "14"
    rows = []
    for fam_dir in sorted(glob.glob(os.path.join(root, "*"))):
        if not os.path.isdir(fam_dir):
            continue
        fam = os.path.basename(fam_dir)
        cands = glob.glob(os.path.join(fam_dir, f"*_{size}.cpfont"))
        if not cands:
            continue
        try:
            r = analyze_file(cands[0])
        except Exception as e:
            print(f"  !! {fam}: {e}", file=sys.stderr); continue
        if r:
            rows.append((fam, r))
    # Rank by inter-glyph width spread, then gray-edge load.
    rows.sort(key=lambda kv: (kv[1]["widthSpread"], kv[1]["grayLoad"]), reverse=True)
    print(f"\nVertical-stem consistency @ size {size} (regular). Worst first.\n")
    print(f"{'family':<26} {'spread':>7} {'grayLoad':>9} {'solidPx':>10} {'meanCov':>8}")
    print("-" * 64)
    for fam, r in rows:
        print(f"{fam:<26} {r['widthSpread']:>6.2f}p {r['grayLoad']*100:>8.1f}% "
              f"{str(r['solidSet']):>10} {r['meanCov']:>7.2f}p")
    print("\nspread   = stdev of stem width ACROSS letters (inter-glyph inconsistency; higher=worse)")
    print("grayLoad = share of stem ink that is gray, not solid black (the AA edge the e-ink must render)")
    print("solidPx  = distinct solid-black core widths seen across letters ([2,3] = some letters thicker)")

if __name__ == "__main__":
    main()
