# Generate a digits-only large Source Serif Bold for Clockface.
# Invokes fontconvert.py with monkeypatched intervals (space, 0-9, colon, replacement).
import sys
from pathlib import Path

scripts = Path(__file__).resolve().parent
sys.path.insert(0, str(scripts))

# Patch argv for fontconvert argparse
sys.argv = [
    "fontconvert.py",
    "sourceserif4_72_clock",
    "72",
    str(scripts.parent / "builtinFonts" / "source" / "SourceSerif4" / "SourceSerif4-Bold.ttf"),
    "--2bit",
    "--compress",
    "--pnum",
]

# Load fontconvert module source and replace intervals block before exec
src_path = scripts / "fontconvert.py"
src = src_path.read_text(encoding="utf-8")
# Replace the big intervals list with clock-only glyphs.
import re
new_intervals = """intervals = [
    (0x0020, 0x0020),  # space
    (0x0030, 0x003A),  # 0-9 and colon
    (0xFFFD, 0xFFFD),  # replacement
]
"""
src2 = re.sub(
    r"intervals = \[.*?\]\n\nadd_ints",
    new_intervals + "\nadd_ints",
    src,
    count=1,
    flags=re.S,
)
if src2 == src:
    raise SystemExit("Failed to patch intervals in fontconvert.py")
out_path = scripts.parent / "builtinFonts" / "sourceserif4_72_clock.h"
# fontconvert prints to stdout
import io
from contextlib import redirect_stdout
buf = io.StringIO()
# Execute patched source
g = {"__name__": "__main__", "__file__": str(src_path)}
with redirect_stdout(buf):
    exec(compile(src2, str(src_path), "exec"), g)
text = buf.getvalue()
out_path.write_text(text, encoding="utf-8")
print(f"Wrote {out_path} ({out_path.stat().st_size} bytes)", file=sys.stderr)
print(f"Lines: {text.count(chr(10))}", file=sys.stderr)
