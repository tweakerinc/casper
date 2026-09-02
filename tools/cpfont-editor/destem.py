#!/usr/bin/env python3
"""Targeted vertical-stem/gap crispening for 2-bit glyph bitmaps.

Two artifacts, one cause (ambiguous mid-gray in narrow vertical features):
  A) GAP   : a vertical gray column flanked L+R by solid black -> a separation
             AA muddied. Cut to white (re-open the gap).  Fixes the " blob.
  B) FRINGE: a vertical gray column adjacent (one side) to a solid-black stem.
             Round it: dark-gray(2)->black(3), light-gray(1)->white(0).
             Fixes the n stem AA.

VERTICAL COHERENCE is the guard: only act on a gray pixel whose column is gray
for a sustained vertical run (>=MINRUN rows). AA on curves/diagonals never forms
such a run, so it is left untouched.
"""
MINRUN  = 2         # min gray run to count as a feature (not a lone AA pixel)
MINSTEM = 5         # min black-column height to be considered a stem segment

def _run(L, h, y, x, pred):
    """length of the consecutive vertical run in column x through row y where pred holds."""
    if not pred(L[y][x]): return 0
    n = 1
    yy = y-1
    while yy >= 0 and pred(L[yy][x]): n += 1; yy -= 1
    yy = y+1
    while yy < h and pred(L[yy][x]): n += 1; yy += 1
    return n

def _col_gray_run(L, h, y, x):
    return _run(L, h, y, x, lambda v: v in (1, 2))

def _black_stem(L, h, y, x):
    """is column x at row y part of a vertical black STEM segment?

    A stem segment is a black column run of >= MINSTEM rows with at least one
    FREE END (white or glyph edge directly beyond it). A closed curve's vertical
    side (o, c, e, g) is bounded by black at both ends -> not a stem -> spared.
    A terminal stem (Y baseline, h/n top) has a free end -> caught, regardless
    of length. This separates Y from o, which length alone cannot.
    """
    if x < 0 or x >= len(L[y]) or L[y][x] != 3: return False
    y0 = y
    while y0 > 0 and L[y0-1][x] == 3: y0 -= 1
    y1 = y
    while y1 < h-1 and L[y1+1][x] == 3: y1 += 1
    if (y1 - y0 + 1) < MINSTEM: return False
    free_top = (y0 == 0)   or (L[y0-1][x] == 0)
    free_bot = (y1 == h-1) or (L[y1+1][x] == 0)
    return free_top or free_bot

def _gray_run_bounds(L, h, y, x):
    y0 = y
    while y0 > 0 and L[y0-1][x] in (1, 2): y0 -= 1
    y1 = y
    while y1 < h-1 and L[y1+1][x] in (1, 2): y1 += 1
    return y0, y1

def _outer_clear(L, h, w, y, x, step):
    """True if, past the black stem starting at x+step (away from the gray at x),
    the column just beyond the stem is white over the gray's vertical run.

    A straight stem has white outside it; a curved stroke BULGES, putting black
    outside in the middle rows. This is the line-vs-curve test that length and
    free-end both miss (o's outer bowl column mimics a stem)."""
    nx = x + step
    while 0 <= nx < w and L[y][nx] == 3:   # skip across the full stem width
        nx += step
    if not (0 <= nx < w):                   # stem runs to glyph edge -> clean
        return True
    y0, y1 = _gray_run_bounds(L, h, y, x)
    return all(L[yy][nx] != 3 for yy in range(y0, y1+1))

def crispen(L, gaps_only=False):
    """gaps_only=True applies ONLY the safe gap-reopen pass (gray flanked by
    black on both sides -> white). It never touches stems or curves, so it keeps
    all anti-aliasing intact and just un-muddies blobbed gaps like the " quote.
    The fringe pass (stem crisping) is fragile and removes stem AA -> off here."""
    h = len(L); w = len(L[0]) if h else 0
    out = [row[:] for row in L]
    for y in range(h):
        for x in range(w):
            lv = L[y][x]
            if lv not in (1, 2):           # only ambiguous gray is a candidate
                continue
            if _col_gray_run(L, h, y, x) < MINRUN:
                continue                   # lone AA pixel -> leave
            lblack = _black_stem(L, h, y, x-1) and _outer_clear(L, h, w, y, x, -1)
            rblack = _black_stem(L, h, y, x+1) and _outer_clear(L, h, w, y, x, +1)
            if lblack and rblack:
                out[y][x] = 0              # GAP: re-open (always safe)
            elif (not gaps_only) and (lblack ^ rblack):
                out[y][x] = 3 if lv == 2 else 0   # FRINGE: stem crisp (off in gaps_only)
    return out

# ---- demo / regression harness ----
if __name__ == "__main__":
    import os
    from cpfont_engine import CpFont
    p = os.path.expanduser("~/BulkDocuments/crosspoint-backup/.fonts/LexicaUltralegible/LexicaUltralegible_12.cpfont")
    cf = CpFont(p)
    R = {0:' ',1:'.',2:'o',3:'#'}
    def show(L, lbl):
        print(f"  {lbl}")
        for row in L:
            print("    " + "".join(R[v] for v in row))
    targets = ['"', '“', 'n', 'h', 'm', 'l', 'o', 'e', 'a', 's', 'x', 'v', 'w', 'r', 'u']
    for ch in targets:
        d = cf.get_levels(0, ord(ch))
        if not d or not d["levels"]: continue
        before = d["levels"]; after = crispen(before)
        changed = sum(1 for y in range(len(before)) for x in range(len(before[0])) if before[y][x]!=after[y][x])
        print(f"\n=== '{ch}' {d['w']}x{d['h']}  ({changed} px changed) ===")
        # side by side
        bl = ["".join(R[v] for v in row) for row in before]
        al = ["".join(R[v] for v in row) for row in after]
        wpad = max(len(s) for s in bl)
        print(f"  {'BEFORE':<{wpad+2}}  AFTER")
        for i in range(len(bl)):
            print(f"  {bl[i]:<{wpad+2}}  {al[i]}")
