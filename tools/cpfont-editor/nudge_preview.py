#!/usr/bin/env python3
"""Nudge preview: render each font's pangram at a range of ppems (gap-fixed), so
you can click the size that looks least strange per font and export a ppem config
that build-sd-fonts.py consumes (--ppem-config).

    python3 nudge_preview.py --size 12 -o nudge.html        # reads build/sd-fonts.yaml
    # pick in the browser, Export config -> picks.json
    # then: cd build && python3 build-sd-fonts.py --ppem-config ../picks.json ...

Resolves sources (download + variable-font instancing) exactly like the build,
by reading sd-fonts.yaml and reusing build-sd-fonts.py's resolver. Needs
freetype + fonttools + pyyaml.
"""
import os, sys, json, argparse, importlib.util
import freetype, yaml

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
sys.path.insert(0, BUILD)
import fontconvert_sdcard as fcs
import destem

def _load_bsf():
    spec = importlib.util.spec_from_file_location("bsf", os.path.join(BUILD, "build-sd-fonts.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

PANGRAM = "The quick brown fox jumps over the lazy dog"

def q(v):
    n = v >> 4
    return 3 if n >= 12 else 2 if n >= 8 else 1 if n >= 4 else 0

def glyph_levels(face, ch):
    face.load_char(ch, freetype.FT_LOAD_RENDER)
    b = face.glyph.bitmap; w, h, p, buf = b.width, b.rows, b.pitch, b.buffer
    lv = [[q(buf[y * p + x]) for x in range(w)] for y in range(h)]
    if w and h:
        lv = destem.crispen(lv, gaps_only=True)        # the gap-fix correction
    g = face.glyph
    return lv, w, h, g.bitmap_left, g.bitmap_top, g.advance.x >> 6

def render_pangram(face, ppem, text):
    face.set_pixel_sizes(0, ppem)
    asc = face.size.ascender >> 6
    desc = face.size.descender >> 6
    lineH = asc - desc; pad = 2
    items = []; pen = 0; maxx = 0
    for ch in text:
        lv, w, h, left, top, adv = glyph_levels(face, ch)
        if w and h:
            items.append((pen + left, asc - top, lv, w, h)); maxx = max(maxx, pen + left + w)
        pen += adv
    W = max(pen, maxx) + pad; H = lineH + pad * 2
    grid = [[0] * W for _ in range(H)]
    for x0, y0, lv, w, h in items:
        for yy in range(h):
            gy = y0 + yy + pad
            if 0 <= gy < H:
                for xx in range(w):
                    gx = x0 + xx
                    if 0 <= gx < W and lv[yy][xx] > grid[gy][gx]:
                        grid[gy][gx] = lv[yy][xx]
    return W, H, "".join(str(v) for r in grid for v in r)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=os.path.join(BUILD, "sd-fonts.yaml"))
    ap.add_argument("--only", help="comma-separated family names")
    ap.add_argument("--size", type=int, default=12, help="size slot to tune (default 12=Small)")
    ap.add_argument("--range", default="-2,2", help="extra ppem margin below/above the "
                    "nominal..suggestion span, lo,hi (default -2,2)")
    ap.add_argument("-o", "--output", default="nudge.html")
    a = ap.parse_args()
    bsf = _load_bsf()
    nominal = round(a.size * 150 / 72)
    lo, hi = (int(x) for x in a.range.split(","))
    families = yaml.safe_load(open(a.config))["families"]
    if a.only:
        want = set(a.only.split(","))
        families = [f for f in families if f["name"] in want]
    data = []
    for fam in families:
        name = fam["name"]
        styles = fam.get("styles", {})
        spec = styles.get("regular") or next(iter(styles.values()), None)
        if not spec:
            continue
        try:
            src = bsf.resolve_font_path(spec, name, "regular")
        except Exception as e:
            print(f"  skip {name}: {e}"); continue
        face = freetype.Face(str(src))
        try:
            suggest = fcs.suggest_ppem(str(src), a.size)
        except Exception:
            suggest = nominal
        lo_p = min(nominal, suggest) + lo
        hi_p = max(nominal, suggest) + hi
        cands = []
        for pp in range(lo_p, hi_p + 1):
            W, H, px = render_pangram(face, pp, PANGRAM)
            cands.append({"ppem": pp, "pt": round(pp * 72 / 150, 1), "w": W, "h": H, "px": px})
        data.append({"name": name, "nominal": nominal, "suggest": suggest, "cands": cands})
        print(f"  {name}: ppem {lo_p}..{hi_p} ({len(cands)} cands), suggest {suggest}, nominal {nominal}")
    html = HTML.replace("/*DATA*/", json.dumps(data)).replace("__SIZE__", str(a.size))
    with open(a.output, "w") as f:
        f.write(html)
    print(f"\nwrote {a.output} ({len(data)} fonts). Open it, pick per font, Export config.")

HTML = r"""<!doctype html><html><head><meta charset=utf-8><title>Nudge preview</title>
<style>
 body{font:14px system-ui,sans-serif;margin:0;background:#f4f4f4;color:#222}
 header{position:sticky;top:0;background:#fff;border-bottom:1px solid #ccc;padding:10px 16px;z-index:9}
 header h1{font-size:16px;margin:0 0 4px}
 header .ctl{font-size:13px;color:#444} header .ctl *{vertical-align:middle}
 button{font:13px system-ui;padding:5px 12px;cursor:pointer}
 .font{background:#fff;margin:12px 16px;border:1px solid #ddd;border-radius:6px}
 .font h2{font-size:14px;margin:0;padding:8px 12px;background:#fafafa;border-bottom:1px solid #eee}
 .cand{display:flex;align-items:center;gap:10px;padding:5px 12px;border-bottom:1px solid #f4f4f4;cursor:pointer}
 .cand:last-child{border-bottom:none}
 .cand:hover{background:#eef}
 .cand.sel{background:#dfe9ff;outline:2px solid #06c;outline-offset:-2px}
 .cand .lab{min-width:120px;font-size:12px;color:#666;font-variant-numeric:tabular-nums}
 .cand .badge{display:inline-block;font-size:10px;padding:1px 5px;border-radius:8px;margin-left:4px}
 .badge.sg{background:#0a7;color:#fff} .badge.nm{background:#999;color:#fff}
 canvas{image-rendering:pixelated;display:block}
 #out{width:100%;height:120px;font-family:monospace;font-size:12px}
 dialog{width:60%;border:1px solid #888;border-radius:8px}
</style></head><body>
<header>
 <h1>Nudge preview — pick the least-strange size per font (slot __SIZE__)</h1>
 <div class=ctl>"quick brown fox" · gap-fixed · click a row to pick · green=auto-suggested, grey=nominal
   &nbsp; zoom <input type=range id=zoom min=2 max=6 value=3>
   &nbsp;<label><input type=checkbox id=invert> dark</label>
   &nbsp;<button id=export>Export config &#9662;</button>
 </div>
</header>
<div id=list></div>
<dialog id=dlg><h3 style="margin-top:0">ppem config (save as picks.json)</h3>
 <textarea id=out></textarea>
 <div style="margin-top:8px"><button id=dl>Download picks.json</button> <button id=close>Close</button></div>
</dialog>
<script>
const DATA=/*DATA*/, SIZE="__SIZE__";
const picks={};
function draw(cv,g,sc,inv){cv.width=g.w*sc;cv.height=g.h*sc;const c=cv.getContext('2d');
 c.fillStyle=inv?'#111':'#fff';c.fillRect(0,0,cv.width,cv.height);
 const ink=inv?[null,'#555','#aaa','#fff']:[null,'#bbb','#666','#000'];let i=0;
 for(let y=0;y<g.h;y++)for(let x=0;x<g.w;x++){const v=+g.px[i++];if(v){c.fillStyle=ink[v];c.fillRect(x*sc,y*sc,sc,sc);}}}
function render(){
 const sc=+zoom.value, inv=invert.checked, L=document.getElementById('list');L.innerHTML='';
 DATA.forEach(f=>{
   if(!(f.name in picks)) picks[f.name]=f.suggest;
   const box=document.createElement('div');box.className='font';box.innerHTML=`<h2>${f.name}</h2>`;
   f.cands.forEach(g=>{
     const row=document.createElement('div');row.className='cand'+(picks[f.name]===g.ppem?' sel':'');
     const lab=document.createElement('div');lab.className='lab';
     lab.innerHTML=`ppem ${g.ppem} &middot; ${g.pt}pt`+
       (g.ppem===f.suggest?'<span class="badge sg">suggest</span>':'')+
       (g.ppem===f.nominal?'<span class="badge nm">nominal</span>':'');
     const cv=document.createElement('canvas');draw(cv,g,sc,inv);
     row.appendChild(lab);row.appendChild(cv);
     row.onclick=()=>{picks[f.name]=g.ppem;render();};
     box.appendChild(row);
   });
   L.appendChild(box);
 });
}
function config(){const o={};for(const f in picks){o[f]={};o[f][SIZE]=picks[f];}return JSON.stringify(o,null,2);}
zoom.oninput=render;invert.onchange=render;
document.getElementById('export').onclick=()=>{document.getElementById('out').value=config();document.getElementById('dlg').showModal();};
document.getElementById('close').onclick=()=>document.getElementById('dlg').close();
document.getElementById('dl').onclick=()=>{const b=new Blob([config()],{type:'application/json'});
 const a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='picks.json';a.click();};
render();
</script></body></html>"""

if __name__ == "__main__":
    main()
