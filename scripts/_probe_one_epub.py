import re, zipfile
from pathlib import Path
p = Path(r"e:/Calibre eBooks/Matt Dinniman/The Butcher's Masquerade (13)/The Butcher's Masquerade - Matt Dinniman.epub")
z = zipfile.ZipFile(p)
names = z.namelist()
html = [n for n in names if n.lower().endswith((".xhtml",".html",".htm"))]
css = [n for n in names if n.lower().endswith(".css")]
imgs = [n for n in names if n.lower().endswith((".jpg",".jpeg",".png",".gif",".svg",".webp"))]
text = ""
for n in css[:20]:
    try: text += z.read(n).decode("utf-8","ignore")
    except Exception: pass
for n in html[:15]:
    try: text += z.read(n).decode("utf-8","ignore")[:300000]
    except Exception: pass

def has(pat):
    return bool(re.search(pat, text, re.I))

flags = ["float","table","dropcap","smallcaps","fontsize","lineheight","strikethrough","columns","position","gif","svg"]
pats = {
 "float": r"float\s*:",
 "table": r"<table|display\s*:\s*table",
 "dropcap": r"drop.?cap|::first-letter|first-letter",
 "smallcaps": r"small-caps|font-variant",
 "fontsize": r"font-size\s*:",
 "lineheight": r"line-height\s*:",
 "strikethrough": r"line-through",
 "columns": r"column-count|columns\s*:",
 "position": r"position\s*:\s*(absolute|fixed|relative)",
 "gif": None,
 "svg": None,
}
hit = {}
for k,pat in pats.items():
    if k=="gif": hit[k]=any(n.lower().endswith(".gif") for n in imgs)
    elif k=="svg": hit[k]=any(n.lower().endswith(".svg") for n in imgs)
    else: hit[k]=has(pat)
max_html = max((z.getinfo(n).file_size for n in html), default=0)
print("file", p.name)
print("MB", round(p.stat().st_size/1e6,2), "html", len(html), "css", len(css), "img", len(imgs), "maxHtmlKB", round(max_html/1024,1))
print("HIT", {k:v for k,v in hit.items() if v})
print("MISS", [k for k,v in hit.items() if not v])
print("--- CSS hits ---")
for n in css:
    try: c = z.read(n).decode("utf-8","ignore")
    except Exception: continue
    print("CSS", n, "len", len(c))
    for m in re.finditer(r".{0,30}(font-size|line-height|font-variant|float\s*:|h1|h2|h3).{0,50}", c, re.I):
        print(" ", " ".join(m.group(0).split())[:140])
print("--- headings ---")
for n in html[:20]:
    try: h = z.read(n).decode("utf-8","ignore")[:100000]
    except Exception: continue
    hs = re.findall(r"<(h[1-6])\b[^>]*>([^<]{0,50})", h, re.I)
    if hs:
        print(Path(n).name, [(a,b.strip()[:40]) for a,b in hs[:5]])
# class with font-size
print("--- unique font-size decls (sample) ---")
fs = re.findall(r"font-size\s*:\s*[^;}{]+", text, re.I)
from collections import Counter
for v,c in Counter(x.strip().lower() for x in fs).most_common(12):
    print(f"  {c:3d}x {v}")
lh = re.findall(r"line-height\s*:\s*[^;}{]+", text, re.I)
print("--- line-height decls ---")
for v,c in Counter(x.strip().lower() for x in lh).most_common(8):
    print(f"  {c:3d}x {v}")
