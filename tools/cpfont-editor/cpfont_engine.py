#!/usr/bin/env python3
"""cpfont v4 read / edit / rebuild engine.

Supports editing glyph pixel LEVELS and RESIZING glyphs (changing w/h, bearings,
advance). Because a resize changes a glyph's packed length, save() fully REBUILDS
the file: glyph tables get fresh data offsets and the bitmap sections are
re-concatenated. Opaque sections (intervals, kerning, ligatures) are preserved
byte-for-byte. parse -> save with no edits reproduces the input exactly.

Format (little-endian), per fontconvert_sdcard.py:
  header 32B: magic(8) version(H) flags(H) styleCount(B) reserved(19s)
  TOC/style 32B: styleId(B) pad(3) intervalCount(I) glyphCount(I)
     advanceY(B) ascender(h) descender(h) kernL(H) kernR(H)
     kernLCls(B) kernRCls(B) ligCount(B) dataOffset(I) reserved(4)
  per style @dataOffset: intervals, glyphs, kernLeft, kernRight,
     kernMatrix, ligatures, bitmaps  (concatenated, no padding)
"""
import struct

MAGIC      = b"CPFONT\x00\x00"
HEADER_FMT = "<8sHHB19s"
TOC_FMT    = "<B3xIIBhhHHBBBI4x"
GLYPH_FMT  = "<BBHhhH2xI"   # width,height,advX,left,top,dataLen,(pad2),dataOff
HEADER_SZ  = 32
TOC_SZ     = 32

class CpFont:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.buf = bytearray(f.read())
        self._parse()

    def _parse(self):
        b = self.buf
        magic, self.version, self.flags, style_count, self._reserved = \
            struct.unpack_from(HEADER_FMT, b, 0)
        if magic != MAGIC:
            raise ValueError("not a cpfont")
        self.styles = []
        for i in range(style_count):
            (sid, ic, gc, advY, asc, desc, kl, kr, klc, krc, lig, doff) = \
                struct.unpack_from(TOC_FMT, b, HEADER_SZ + i*TOC_SZ)
            o = doff
            intervals_b = bytes(b[o:o+ic*12]);            o += ic*12
            glyph_tbl_o = o;                              o += gc*16
            kern_l_b    = bytes(b[o:o+kl*3]);             o += kl*3
            kern_r_b    = bytes(b[o:o+kr*3]);             o += kr*3
            kern_m_b    = bytes(b[o:o+klc*krc]);          o += klc*krc
            lig_b       = bytes(b[o:o+lig*8]);            o += lig*8
            bm_start    = o
            glyphs = []
            for gi in range(gc):
                w,h,adv,left,top,dlen,doff2 = struct.unpack_from(GLYPH_FMT, b, glyph_tbl_o+gi*16)
                data = bytes(b[bm_start+doff2: bm_start+doff2+dlen])
                glyphs.append(dict(w=w,h=h,adv=adv,left=left,top=top,data=data))
            cp2gi = {}
            for j in range(ic):
                s,e,off = struct.unpack_from("<III", intervals_b, j*12)
                for cp in range(s, e+1):
                    cp2gi[cp] = off + (cp - s)
            self.styles.append(dict(
                sid=sid, advanceY=advY, ascender=asc, descender=desc,
                klc=klc, krc=krc,
                intervals=intervals_b, kern_l=kern_l_b, kern_r=kern_r_b,
                kern_m=kern_m_b, lig=lig_b, glyphs=glyphs, cp2gi=cp2gi))

    # ---------- bitmap codec (2-bit, 4px/byte, MSB-first) ----------
    @staticmethod
    def _decode(data, w, h):
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                p = y*w + x
                row.append((data[p>>2] >> ((3-(p&3))*2)) & 3)
            rows.append(row)
        return rows

    @staticmethod
    def _encode(levels, w, h):
        out = bytearray(); px = 0
        for y in range(h):
            for x in range(w):
                px = (px << 2) | (levels[y][x] & 3)
                if (y*w + x) % 4 == 3:
                    out.append(px); px = 0
        if (w*h) % 4 != 0:
            px <<= (4 - (w*h) % 4) * 2
            out.append(px)
        return bytes(out)

    # ---------- accessors ----------
    def get(self, si, cp):
        st = self.styles[si]; gi = st["cp2gi"].get(cp)
        if gi is None: return None
        g = st["glyphs"][gi]
        levels = self._decode(g["data"], g["w"], g["h"]) if g["w"] and g["h"] else []
        return dict(w=g["w"], h=g["h"], adv=g["adv"], left=g["left"], top=g["top"], levels=levels)

    def set_glyph(self, si, cp, levels, w, h, adv=None, left=None, top=None):
        """Replace a glyph's bitmap + (optionally) metadata. w/h may differ from
        the original (resize); save() rebuilds offsets accordingly."""
        st = self.styles[si]; gi = st["cp2gi"].get(cp)
        if gi is None: raise KeyError(cp)
        g = st["glyphs"][gi]
        g["w"], g["h"] = w, h
        g["data"] = self._encode(levels, w, h) if w and h else b""
        if adv  is not None: g["adv"]  = adv
        if left is not None: g["left"] = left
        if top  is not None: g["top"]  = top

    # ---------- rebuild ----------
    def _serialize(self):
        style_count = len(self.styles)
        style_blobs = []; toc = []
        for st in self.styles:
            glyph_tbl = bytearray(); bitmaps = bytearray(); off = 0
            for g in st["glyphs"]:
                dlen = len(g["data"])
                glyph_tbl += struct.pack(GLYPH_FMT, g["w"], g["h"], g["adv"],
                                         g["left"], g["top"], dlen, off)
                bitmaps += g["data"]; off += dlen
            blob = (st["intervals"] + bytes(glyph_tbl) + st["kern_l"] + st["kern_r"]
                    + st["kern_m"] + st["lig"] + bytes(bitmaps))
            style_blobs.append(blob)
            toc.append(dict(sid=st["sid"],
                            ic=len(st["intervals"])//12, gc=len(st["glyphs"]),
                            advY=st["advanceY"], asc=st["ascender"], desc=st["descender"],
                            kl=len(st["kern_l"])//3, kr=len(st["kern_r"])//3,
                            klc=st["klc"], krc=st["krc"], lig=len(st["lig"])//8))
        out = bytearray()
        out += struct.pack(HEADER_FMT, MAGIC, self.version, self.flags, style_count, self._reserved)
        data_off = HEADER_SZ + style_count*TOC_SZ
        offs = []
        for blob in style_blobs:
            offs.append(data_off); data_off += len(blob)
        for t, o in zip(toc, offs):
            out += struct.pack(TOC_FMT, t["sid"], t["ic"], t["gc"], t["advY"], t["asc"],
                               t["desc"], t["kl"], t["kr"], t["klc"], t["krc"], t["lig"], o)
        for blob in style_blobs:
            out += blob
        return bytes(out)

    def save(self, path=None):
        with open(path or self.path, "wb") as f:
            f.write(self._serialize())


if __name__ == "__main__":
    import sys, os
    p = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        "~/BulkDocuments/crosspoint-backup/.fonts/LexicaUltralegible/LexicaUltralegible_12.cpfont")
    cf = CpFont(p)
    print(f"v{cf.version} styles={[s['sid'] for s in cf.styles]} glyphs/style={len(cf.styles[0]['glyphs'])}")
    with open(p, "rb") as f: orig = f.read()
    rebuilt = cf._serialize()
    print(f"rebuild roundtrip: {'OK identical' if rebuilt == orig else f'MISMATCH ({len(rebuilt)} vs {len(orig)})'}")
    bad = sum(1 for st in cf.styles for g in st["glyphs"] if g["w"] and g["h"]
              and cf._encode(cf._decode(g["data"], g["w"], g["h"]), g["w"], g["h"]) != g["data"])
    print(f"bitmap codec roundtrip: {bad} mismatches")
    d = cf.get(0, ord('"')); w,h = d["w"], d["h"]
    cf.set_glyph(0, ord('"'), [row + [0] for row in d["levels"]], w+1, h, adv=d["adv"]+16)
    cf.save("/tmp/resize_test.cpfont")
    cf2 = CpFont("/tmp/resize_test.cpfont"); d2 = cf2.get(0, ord('"'))
    print(f"resize: quote {w}x{h} -> {d2['w']}x{d2['h']} adv {d['adv']/16}->{d2['adv']/16}; reparse OK")
    z = cf2.get(0, ord('z'))
    print(f"later glyph 'z' still valid: {z['w']}x{z['h']}")
    os.remove("/tmp/resize_test.cpfont")
