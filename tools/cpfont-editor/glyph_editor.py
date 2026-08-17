#!/usr/bin/env python3
"""Local web pixel-editor for cpfont glyphs. Stdlib only.

    python3 glyph_editor.py path/to/Font_12.cpfont [--port 8765]

Browse all glyphs as a preview grid, click one to edit its 2-bit pixels, RESIZE
the canvas (add/remove rows/cols, tweak advance/bearings), see it live in a
word-context strip, and Save back into the .cpfont (rebuilds offsets so resizes
are safe). Edits a COPY is recommended -- a font rebuild would overwrite.
"""
import sys, os, json, argparse, unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from cpfont_engine import CpFont
try:
    import destem
    HAVE_DESTEM = True
except Exception:
    HAVE_DESTEM = False

STYLE_NAMES = {0: "regular", 1: "bold", 2: "italic", 3: "bolditalic"}

def category(cp):
    c = unicodedata.category(chr(cp))
    return {'L':'letter','N':'digit','P':'punct','S':'symbol','Z':'space'}.get(c[0], 'other')

def px_string(levels):
    return "".join(str(v) for row in levels for v in row)

class Handler(BaseHTTPRequestHandler):
    font = None; fontpath = ""
    def log_message(self, *a): pass
    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        elif isinstance(body, str): body = body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/":
            return self._send(200, HTML, "text/html")
        if u.path == "/api/info":
            si = int(q.get("style", ["0"])[0]); st = self.font.styles[si]
            glyphs = []
            for cp in sorted(st["cp2gi"].keys()):
                d = self.font.get(si, cp)
                if not d: continue
                glyphs.append({"cp": cp, "label": chr(cp) if 0x20 < cp < 0x7f else f"U+{cp:04X}",
                               "ch": chr(cp), "w": d["w"], "h": d["h"], "cat": category(cp),
                               "px": px_string(d["levels"])})
            return self._send(200, {"family": os.path.basename(self.fontpath),
                                    "styles": [{"sid": s["sid"], "name": STYLE_NAMES.get(s["sid"], str(s["sid"]))}
                                               for s in self.font.styles],
                                    "ascender": st["ascender"], "descender": st["descender"],
                                    "haveAuto": HAVE_DESTEM, "glyphs": glyphs})
        if u.path == "/api/glyph":
            si = int(q.get("style", ["0"])[0]); cp = int(q["cp"][0])
            d = self.font.get(si, cp); st = self.font.styles[si]
            ctx = []
            for c in [ord('n'), ord('o'), cp, ord('o'), ord('n')]:
                cd = self.font.get(si, c)
                if cd: ctx.append({"cp": c, **cd})
            return self._send(200, {"cp": cp, **d, "context": ctx,
                                    "ascender": st["ascender"], "descender": st["descender"]})
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        if u.path == "/api/glyph":
            si = int(body["style"]); cp = int(body["cp"])
            self.font.set_glyph(si, cp, body["levels"], int(body["w"]), int(body["h"]),
                                adv=int(body["adv"]), left=int(body["left"]), top=int(body["top"]))
            self.font.save()
            d = self.font.get(si, cp)
            return self._send(200, {"ok": True, "px": px_string(d["levels"]), "w": d["w"], "h": d["h"]})
        if u.path == "/api/gapfix_all" and HAVE_DESTEM:
            ng = 0
            for si, st in enumerate(self.font.styles):
                for cp in list(st["cp2gi"]):
                    d = self.font.get(si, cp)
                    if not d or not d["levels"]: continue
                    A = destem.crispen(d["levels"], gaps_only=True)
                    if A != d["levels"]:
                        self.font.set_glyph(si, cp, A, d["w"], d["h"]); ng += 1
            self.font.save()
            return self._send(200, {"ok": True, "glyphs": ng})
        return self._send(404, {"error": "not found"})

HTML = r"""<!doctype html><html><head><meta charset=utf-8><title>cpfont editor</title>
<style>
 *{box-sizing:border-box} body{font:13px system-ui,sans-serif;margin:0;display:flex;height:100vh;color:#222}
 #side{width:340px;border-right:1px solid #ccc;display:flex;flex-direction:column;min-width:340px}
 #side h1{font-size:13px;margin:8px;font-weight:600}
 #controls{padding:0 8px 8px;border-bottom:1px solid #eee}
 #controls input,#controls select{font:12px system-ui}
 #grid{overflow:auto;flex:1;display:flex;flex-wrap:wrap;align-content:flex-start;gap:2px;padding:6px}
 .cell{width:48px;height:60px;border:1px solid #eee;display:flex;flex-direction:column;align-items:center;
       justify-content:center;cursor:pointer;background:#fff}
 .cell:hover{background:#eef;border-color:#88a}.cell.sel{background:#dde;border-color:#06c}
 .cell canvas{image-rendering:pixelated}
 .cell .lab{font-size:9px;color:#888;margin-top:2px;max-width:46px;overflow:hidden;white-space:nowrap}
 #main{flex:1;display:flex;flex-direction:column;padding:14px;gap:12px;overflow:auto}
 #pal button{width:28px;height:28px;margin-right:4px;border:2px solid #888;vertical-align:middle}
 #pal button.on{outline:3px solid #06c}
 #grid2{border-collapse:collapse;cursor:crosshair;user-select:none}
 #grid2 td{width:18px;height:18px;border:1px solid #e8e8e8;padding:0}
 .row{display:flex;gap:22px;align-items:flex-start;flex-wrap:wrap}
 canvas.prev{border:1px solid #ccc;image-rendering:pixelated}
 button.act{padding:6px 12px;font-size:13px;margin-right:6px}
 .lbl{font-size:11px;color:#666;margin-bottom:3px}
 #resize button{width:30px;height:24px;margin:1px}
 #meta input{width:48px}
 .grp{border:1px solid #eee;padding:8px;border-radius:5px}
 #status{color:#080;margin-left:8px}
</style></head><body>
<div id=side>
 <h1>cpfont editor — <span id=fam></span></h1>
 <div id=controls>
   style <select id=style></select>
   filter <select id=filter><option value=all>all</option><option value=letter>letter</option>
     <option value=digit>digit</option><option value=punct>punct</option><option value=symbol>symbol</option></select><br>
   <input id=search placeholder="search char or U+201C" style="width:96%;margin-top:4px">
 </div>
 <div id=grid></div>
</div>
<div id=main>
 <div id=ginfo style="font-weight:600">pick a glyph from the grid →</div>
 <div id=pal>paint:
   <button data-l=0 style="background:#fff"></button>
   <button data-l=1 style="background:#bbb"></button>
   <button data-l=2 style="background:#666"></button>
   <button data-l=3 style="background:#000" class=on></button>
   <span style="color:#888;margin-left:8px">keys 0–3 · drag to paint</span>
 </div>
 <div class=row>
   <div class=grp><div class=lbl>editor</div><table id=grid2></table></div>
   <div class=grp id=resize>
     <div class=lbl>resize canvas</div>
     <table style="border-spacing:2px">
       <tr><td></td><td><button data-op="row+top">+▲</button></td><td></td></tr>
       <tr><td><button data-op="col+left">+◀</button></td>
           <td style="font-size:11px;text-align:center" id=dims>–</td>
           <td><button data-op="col+right">▶+</button></td></tr>
       <tr><td></td><td><button data-op="row+bot">+▼</button></td><td></td></tr>
     </table>
     <div style="margin-top:4px"><span class=lbl>remove</span><br>
       <button data-op="row-top">−▲</button><button data-op="col-left">−◀</button>
       <button data-op="col-right">−▶</button><button data-op="row-bot">−▼</button></div>
     <div id=meta style="margin-top:8px;font-size:11px">
       adv <input id=adv type=number step=0.5> px<br>
       left <input id=left type=number> top <input id=top type=number>
     </div>
   </div>
   <div class=grp>
     <div class=lbl>1× / 4×</div>
     <canvas class=prev id=p1></canvas> <canvas class=prev id=p4></canvas>
     <div class=lbl style="margin-top:8px">context: n o [glyph] o n</div>
     <canvas class=prev id=ctx></canvas>
   </div>
 </div>
 <div>
   <button class=act id=save>Save</button>
   <button class=act id=revert>Revert</button>
   <button class=act id=gapfix style="float:right">Gap-fix all (safe)</button>
   <span id=status></span>
 </div>
</div>
<script>
const SHADE=['#fff','#bbb','#666','#000'];
let info=null, cur=null, paint=3, mouse=false;
document.body.onmouseup=()=>mouse=false;

function px2grid(px,w,h){const g=[];let i=0;for(let y=0;y<h;y++){const r=[];for(let x=0;x<w;x++)r.push(+px[i++]||0);g.push(r);}return g;}
function drawMini(cv,px,w,h){const box=44,sc=w&&h?Math.max(1,Math.floor(Math.min(box/w,box/h))):1;
  cv.width=Math.max(1,w*sc);cv.height=Math.max(1,h*sc);const g=cv.getContext('2d');
  g.fillStyle='#fff';g.fillRect(0,0,cv.width,cv.height);let i=0;
  for(let y=0;y<h;y++)for(let x=0;x<w;x++){const v=+px[i++]||0;if(v){g.fillStyle=SHADE[v];g.fillRect(x*sc,y*sc,sc,sc);}}}

async function loadInfo(){
  const si=document.getElementById('style').value||0;
  info=await(await fetch('/api/info?style='+si)).json();
  document.getElementById('fam').textContent=info.family;
  const ss=document.getElementById('style');
  if(!ss.options.length)info.styles.forEach(s=>ss.add(new Option(s.name,s.sid)));
  if(!info.haveAuto)document.getElementById('gapfix').style.display='none';
  renderGrid();
}
function renderGrid(){
  const filt=document.getElementById('filter').value, q=document.getElementById('search').value.trim().toLowerCase();
  const G=document.getElementById('grid');G.innerHTML='';
  info.glyphs.filter(g=>{
    if(filt!=='all'&&g.cat!==filt)return false;
    if(q){const hex=('u+'+g.cp.toString(16)).toLowerCase();return g.ch===q||g.label.toLowerCase()===q||hex===q||hex.includes(q)||(q.length===1&&g.ch===q);}
    return true;
  }).forEach(g=>{
    const d=document.createElement('div');d.className='cell'+(cur&&cur.cp===g.cp?' sel':'');
    const cv=document.createElement('canvas');drawMini(cv,g.px,g.w,g.h);
    const lab=document.createElement('div');lab.className='lab';lab.textContent=g.label;
    d.appendChild(cv);d.appendChild(lab);d.onclick=()=>loadGlyph(g.cp);G.appendChild(d);
  });
}
async function loadGlyph(cp){
  const si=document.getElementById('style').value||0;
  cur=await(await fetch('/api/glyph?style='+si+'&cp='+cp)).json();
  if(!cur.levels||!cur.levels.length)cur.levels=Array.from({length:cur.h},()=>Array(cur.w).fill(0));
  document.getElementById('status').textContent='';
  refreshMeta(); buildGrid(); drawAll(); renderGrid();
}
function refreshMeta(){
  document.getElementById('ginfo').textContent=
    `${cur.cp>0x20&&cur.cp<0x7f?String.fromCharCode(cur.cp):'U+'+cur.cp.toString(16)}`;
  document.getElementById('dims').textContent=cur.w+'×'+cur.h;
  document.getElementById('adv').value=(cur.adv/16).toFixed(1);
  document.getElementById('left').value=cur.left;
  document.getElementById('top').value=cur.top;
}
function buildGrid(){
  const t=document.getElementById('grid2');t.innerHTML='';
  for(let y=0;y<cur.h;y++){const tr=t.insertRow();
    for(let x=0;x<cur.w;x++){const td=tr.insertCell();td.style.background=SHADE[cur.levels[y][x]];
      const set=()=>{cur.levels[y][x]=paint;td.style.background=SHADE[paint];drawAll();};
      td.onmousedown=e=>{e.preventDefault();mouse=true;set();};
      td.onmouseenter=()=>{if(mouse)set();};}}
}
function drawG(cv,levels,w,h,sc){cv.width=Math.max(1,w*sc);cv.height=Math.max(1,h*sc);const g=cv.getContext('2d');
  g.fillStyle='#fff';g.fillRect(0,0,cv.width,cv.height);
  for(let y=0;y<h;y++)for(let x=0;x<w;x++){const v=levels[y][x];if(v){g.fillStyle=SHADE[v];g.fillRect(x*sc,y*sc,sc,sc);}}}
function drawCtx(cv,sc){const asc=cur.ascender,desc=cur.descender,lineH=asc-desc;
  let items=cur.context,pen=1,tot=items.reduce((a,it)=>a+Math.round(it.adv/16),2);
  cv.width=tot*sc;cv.height=lineH*sc;const g=cv.getContext('2d');g.fillStyle='#fff';g.fillRect(0,0,cv.width,cv.height);
  items.forEach(it=>{const lv=(it.cp===cur.cp)?cur.levels:it.levels;
    const ox=pen+((it.cp===cur.cp?cur.left:it.left)||0),oy=asc-((it.cp===cur.cp?cur.top:it.top)||0);
    const ww=(it.cp===cur.cp?cur.w:it.w),hh=(it.cp===cur.cp?cur.h:it.h);
    if(lv&&lv.length)for(let y=0;y<hh;y++)for(let x=0;x<ww;x++){const v=lv[y][x];if(v){g.fillStyle=SHADE[v];g.fillRect((ox+x)*sc,(oy+y)*sc,sc,sc);}}
    pen+=Math.round((it.cp===cur.cp?cur.adv:it.adv)/16);});
}
function drawAll(){drawG(document.getElementById('p1'),cur.levels,cur.w,cur.h,1);
  drawG(document.getElementById('p4'),cur.levels,cur.w,cur.h,4);drawCtx(document.getElementById('ctx'),3);
  document.getElementById('dims').textContent=cur.w+'×'+cur.h;}

// resize ops keep ink stationary by adjusting bearings
function resize(op){
  if(!cur)return;const L=cur.levels;
  if(op==='col+left'){L.forEach(r=>r.unshift(0));cur.w++;cur.left--;}
  else if(op==='col-left'&&cur.w>1){L.forEach(r=>r.shift());cur.w--;cur.left++;}
  else if(op==='col+right'){L.forEach(r=>r.push(0));cur.w++;}
  else if(op==='col-right'&&cur.w>1){L.forEach(r=>r.pop());cur.w--;}
  else if(op==='row+top'){L.unshift(Array(cur.w).fill(0));cur.h++;cur.top++;}
  else if(op==='row-top'&&cur.h>1){L.shift();cur.h--;cur.top--;}
  else if(op==='row+bot'){L.push(Array(cur.w).fill(0));cur.h++;}
  else if(op==='row-bot'&&cur.h>1){L.pop();cur.h--;}
  refreshMeta();buildGrid();drawAll();
}
document.querySelectorAll('#resize button[data-op]').forEach(b=>b.onclick=()=>resize(b.dataset.op));
document.getElementById('adv').onchange=e=>{cur.adv=Math.round(parseFloat(e.target.value||0)*16);drawAll();};
document.getElementById('left').onchange=e=>{cur.left=parseInt(e.target.value||0);drawAll();};
document.getElementById('top').onchange=e=>{cur.top=parseInt(e.target.value||0);drawAll();};

document.querySelectorAll('#pal button').forEach(b=>b.onclick=()=>{paint=+b.dataset.l;
  document.querySelectorAll('#pal button').forEach(x=>x.classList.remove('on'));b.classList.add('on');});
document.onkeydown=e=>{if(e.target.tagName==='INPUT')return;if(e.key>='0'&&e.key<='3'){paint=+e.key;
  document.querySelectorAll('#pal button').forEach(x=>x.classList.toggle('on',+x.dataset.l===paint));}};

document.getElementById('save').onclick=async()=>{if(!cur)return;
  const si=document.getElementById('style').value||0;
  const r=await(await fetch('/api/glyph',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({style:+si,cp:cur.cp,levels:cur.levels,w:cur.w,h:cur.h,adv:cur.adv,left:cur.left,top:cur.top})})).json();
  const g=info.glyphs.find(g=>g.cp===cur.cp);if(g){g.px=r.px;g.w=r.w;g.h=r.h;}
  renderGrid();document.getElementById('status').textContent='saved ✓';};
document.getElementById('revert').onclick=()=>{if(cur)loadGlyph(cur.cp);};
document.getElementById('gapfix').onclick=async()=>{
  if(!confirm('Apply safe gap-fix to ALL glyphs (all styles) and save?'))return;
  const r=await(await fetch('/api/gapfix_all',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})).json();
  document.getElementById('status').textContent=`gap-fixed ${r.glyphs} glyphs, saved`;
  cur=null;loadInfo();};
document.getElementById('style').onchange=()=>{cur=null;loadInfo();};
['filter','search'].forEach(id=>document.getElementById(id).oninput=renderGrid);
loadInfo();
</script></body></html>"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fontpath"); ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()
    Handler.font = CpFont(a.fontpath); Handler.fontpath = a.fontpath
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"glyph editor: http://127.0.0.1:{a.port}/   editing {a.fontpath}")
    print("Saves rebuild & write the .cpfont in place. Ctrl-C to stop.")
    try: srv.serve_forever()
    except KeyboardInterrupt: print("\nstopped.")

if __name__ == "__main__":
    main()
