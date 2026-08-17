#!/usr/bin/env python3
"""Generate a .cpfont rasterized at a chosen PPEM (pixels-per-em) instead of a
nominal point size, so stem widths land on whole pixels (crisp on low-PPI eink).

A near-1-bit eink panel only renders a font cleanly at the few ppems where its
stems hit integer widths (2px, 3px, ...). Between them you get a muddy gray
fringe. Use --find to sweep and report the clean ppems, then --ppem to build.

    # find Lexica's clean sizes
    python3 rescale_cpfont.py --find lexsrc/LexicaUltralegible-Regular.ttf
    # build a 2px-stem "Small" slot (all styles) into the _12 filename
    python3 rescale_cpfont.py --ppem 22 --out LexicaUltralegible_12.cpfont \
        --regular lexsrc/LexicaUltralegible-Regular.ttf \
        --bold ...-Bold.ttf --italic ...-Italic.ttf --bolditalic ...-BoldItalic.ttf

Requires freetype + fonttools (host build deps already used by the converter).
Reuses fontconvert_sdcard's kerning/ligature/packing.
"""
import sys, os, argparse, statistics as st, tempfile
sys.path.insert(0, "/home/izzie/Desktop/crosspoint-reader/lib/EpdFont/scripts")
import freetype
import fontconvert_sdcard as fcs

def find_clean(src, lo=16, hi=40):
    f = freetype.Face(src)
    print(f"{'ppem':>4} {'stemCov':>7} {'gray%':>6}  verdict")
    for ppem in range(lo, hi+1):
        f.set_pixel_sizes(0, ppem)
        covs=[]; sol=0; gray=0
        for ch in "lihnmru":
            f.load_char(ch, freetype.FT_LOAD_RENDER)
            b=f.glyph.bitmap; w,h,p,buf=b.width,b.rows,b.pitch,b.buffer
            if not w: continue
            for y in range(int(h*0.45), int(h*0.78)):
                row=[buf[y*p+x] for x in range(w)]
                run=[x for x,v in enumerate(row) if v>0]
                if not run: continue
                a,bb=run[0],run[-1]
                covs.append(sum(row[x]/255 for x in range(a,bb+1)))
                sol+=sum(1 for x in range(a,bb+1) if row[x]>=204)
                gray+=sum(1 for x in range(a,bb+1) if 0<row[x]<204)
        if not covs: continue
        cov=st.median(covs); gf=gray/(sol+gray) if sol+gray else 0
        clean = abs(cov-round(cov))<0.18 and gf<0.18
        print(f"{ppem:>4} {cov:>7.2f} {gf*100:>5.0f}%  {'CLEAN ~%dpx'%round(cov) if clean else ''}")

def ppem_rasterize(ppem):
    """Return a fcs.rasterize_font_style replacement that renders at fixed PPEM."""
    def rasterize(fontfile, size, intervals, style_id=0, **kw):
        face = freetype.Face(fontfile); face.set_pixel_sizes(0, ppem)
        val=[]
        for s,e in intervals:
            start=s
            for cp in range(s,e+1):
                if face.get_char_index(cp)==0:
                    if start<cp: val.append((start,cp-1))
                    start=cp+1
            if start<=e: val.append((start,e))
        intervals=val; total=0; glyphs=[]
        for s,e in intervals:
            for cp in range(s,e+1):
                gi=face.get_char_index(cp)
                if gi==0:
                    glyphs.append((fcs.GlyphProps(0,0,0,0,0,0,total,cp), b'')); continue
                face.load_glyph(gi, freetype.FT_LOAD_RENDER)
                b=face.glyph.bitmap; w,h,pitch,buf=b.width,b.rows,b.pitch,b.buffer
                # 8-bit -> 4-bit -> 2-bit (thresholds 4/8/12), continuous pack
                pix=[]; px=0
                for y in range(h):
                    for x in range(w):
                        n=buf[y*pitch+x]>>4
                        lv=3 if n>=12 else 2 if n>=8 else 1 if n>=4 else 0
                        px=(px<<2)|lv
                        if (y*w+x)%4==3: pix.append(px); px=0
                if (w*h)%4!=0: px<<=(4-(w*h)%4)*2; pix.append(px)
                packed=bytes(pix)
                g=fcs.GlyphProps(w,h, fcs.fp4_from_ft16_16(face.glyph.linearHoriAdvance),
                                 face.glyph.bitmap_left, face.glyph.bitmap_top, len(packed), total, cp)
                total+=len(packed); glyphs.append((g,packed))
        face.load_char('|', freetype.FT_LOAD_RENDER)
        advY=fcs.norm_ceil(face.size.height); asc=fcs.norm_ceil(face.size.ascender)
        desc=fcs.norm_floor(face.size.descender)
        cps=set(g.code_point for g,_ in glyphs)
        km=fcs.extract_kerning_fonttools(fontfile, cps, ppem)
        km={k:v for k,v in km.items() if k[0]<=0xFFFF and k[1]<=0xFFFF}
        klc,krc,kmx,klcc,krcc=fcs.derive_kern_classes(km)
        ligs=fcs.extract_ligatures_fonttools(fontfile, cps)[:255]
        return fcs.StyleRasterData(style_id, intervals, glyphs, total, advY, asc, desc,
                                   klc, krc, kmx, klcc, krcc, ligs)
    return rasterize

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--find"); ap.add_argument("--ppem", type=int)
    ap.add_argument("--intervals", default="latin-ext")
    ap.add_argument("--regular"); ap.add_argument("--bold")
    ap.add_argument("--italic"); ap.add_argument("--bolditalic")
    ap.add_argument("--out")
    a=ap.parse_args()
    if a.find:
        find_clean(a.find); return
    styles={}
    for sid,fn in ((0,a.regular),(1,a.bold),(2,a.italic),(3,a.bolditalic)):
        if fn: styles[sid]=fn
    if not styles or not a.ppem or not a.out:
        ap.error("need --ppem, --out and at least --regular")
    fcs.rasterize_font_style = ppem_rasterize(a.ppem)
    intervals=fcs.resolve_intervals(a.intervals)
    fcs.generate_cpfont_multistyle(styles, a.ppem, intervals, a.out)  # 'size' arg unused now
    print(f"\nwrote {a.out} at ppem {a.ppem}, {os.path.getsize(a.out)} bytes")

if __name__=="__main__":
    main()
