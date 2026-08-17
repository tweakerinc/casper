#!/usr/bin/env python3
"""Render a side-by-side pangram preview of the original vs optimized font packs.

Composites the standard pangram from each .cpfont's glyph bitmaps (advance +
bearings, no kerning) and writes a self-contained preview.html that draws them
on scaled canvases so the 2-bit pixels are visible.

    python3 generate_preview.py \
        --original ~/BulkDocuments/crosspoint-fonts \
        --optimized ~/BulkDocuments/crosspoint-fonts-calibrated \
        --size 12 -o preview.html
"""
import os, json, glob, argparse
from cpfont_engine import CpFont

PANGRAM = "The quick brown fox jumps over the lazy dog"

def compose(path, text):
    """Return (w, h, px-string) of the text rendered from a .cpfont (style 0)."""
    cf = CpFont(path)
    st = cf.styles[0]
    asc, desc = st["ascender"], st["descender"]
    lineH = asc - desc
    pad = 2
    # first pass: pen positions + total width
    glyphs = []
    pen = 0
    maxx = 0
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
                    if 0 <= gx < W and row[xx]:
                        if row[xx] > grid[gy][gx]:
                            grid[gy][gx] = row[xx]
    px = "".join(str(v) for r in grid for v in r)
    return W, H, px

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--original", required=True)
    ap.add_argument("--optimized", required=True)
    ap.add_argument("--size", default="12")
    ap.add_argument("-o", "--output", default="preview.html")
    a = ap.parse_args()
    fams = sorted(os.path.basename(d) for d in glob.glob(a.original + "/*") if os.path.isdir(d))
    data = []
    for fam in fams:
        op = f"{a.original}/{fam}/{fam}_{a.size}.cpfont"
        np = f"{a.optimized}/{fam}/{fam}_{a.size}.cpfont"
        if not (os.path.exists(op) and os.path.exists(np)):
            continue
        try:
            ow, oh, opx = compose(op, PANGRAM)
            nw, nh, npx = compose(np, PANGRAM)
        except Exception as e:
            print(f"  skip {fam}: {e}"); continue
        data.append({"name": fam,
                     "orig": {"w": ow, "h": oh, "px": opx},
                     "opt":  {"w": nw, "h": nh, "px": npx}})
        print(f"  {fam}: orig {ow}x{oh}  opt {nw}x{nh}")
    html = HTML_TMPL.replace("/*DATA*/", json.dumps(data)).replace("__SIZE__", a.size)
    with open(a.output, "w") as f:
        f.write(html)
    print(f"\nwrote {a.output} ({len(data)} fonts)")

HTML_TMPL = r"""<!doctype html><html><head><meta charset=utf-8><title>CrossPoint font preview</title>
<style>
 body{font:14px system-ui,sans-serif;margin:0;background:#f4f4f4;color:#222}
 header{position:sticky;top:0;background:#fff;border-bottom:1px solid #ccc;padding:10px 16px;z-index:9}
 header h1{font-size:16px;margin:0 0 4px}
 header .sub{color:#666;font-size:12px}
 header label{margin-left:16px;font-size:13px}
 .font{background:#fff;margin:12px 16px;border:1px solid #ddd;border-radius:6px;overflow:hidden}
 .font h2{font-size:14px;margin:0;padding:8px 12px;background:#fafafa;border-bottom:1px solid #eee}
 .pair{display:flex;flex-direction:column}
 .variant{padding:8px 12px;border-bottom:1px solid #f2f2f2}
 .variant:last-child{border-bottom:none}
 .variant .tag{font-size:11px;color:#888;margin-bottom:4px;text-transform:uppercase;letter-spacing:.04em}
 .variant.opt .tag{color:#0a7}
 canvas{image-rendering:pixelated;display:block;max-width:100%}
</style></head><body>
<header>
 <h1>CrossPoint font pack — original vs stem-optimized (Small / size __SIZE__)</h1>
 <div class=sub>"The quick brown fox jumps over the lazy dog" · composited from each .cpfont · advance+bearings, no kerning
   <label>zoom <input type=range id=zoom min=2 max=6 value=3></label>
   <label><input type=checkbox id=invert> dark</label></div>
</header>
<div id=list></div>
<script>
const DATA=/*DATA*/;
function draw(cv,g,sc,invert){
  cv.width=g.w*sc; cv.height=g.h*sc;
  const ctx=cv.getContext('2d');
  ctx.fillStyle=invert?'#111':'#fff'; ctx.fillRect(0,0,cv.width,cv.height);
  const ink=invert?[null,'#555','#aaa','#fff']:[null,'#bbb','#666','#000'];
  let i=0;
  for(let y=0;y<g.h;y++)for(let x=0;x<g.w;x++){const v=+g.px[i++];if(v){ctx.fillStyle=ink[v];ctx.fillRect(x*sc,y*sc,sc,sc);}}
}
function render(){
  const sc=+document.getElementById('zoom').value, inv=document.getElementById('invert').checked;
  const L=document.getElementById('list'); L.innerHTML='';
  DATA.forEach(f=>{
    const box=document.createElement('div'); box.className='font';
    box.innerHTML=`<h2>${f.name}</h2>`;
    const pair=document.createElement('div'); pair.className='pair';
    [['original',f.orig,''],['optimized',f.opt,'opt']].forEach(([tag,g,cls])=>{
      const v=document.createElement('div'); v.className='variant '+cls;
      const t=document.createElement('div'); t.className='tag'; t.textContent=`${tag} — ${g.w}×${g.h}px`;
      const cv=document.createElement('canvas'); draw(cv,g,sc,inv);
      v.appendChild(t); v.appendChild(cv); pair.appendChild(v);
    });
    box.appendChild(pair); L.appendChild(box);
  });
}
document.getElementById('zoom').oninput=render;
document.getElementById('invert').onchange=render;
render();
</script></body></html>"""

if __name__ == "__main__":
    main()
