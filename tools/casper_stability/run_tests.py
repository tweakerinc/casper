#!/usr/bin/env python3
"""Compile and execute actual reader sources with deterministic host HAL/fonts."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="casper-rc-") as d:
    exe = Path(d) / "regression"
    includes = ["tools/casper_stability/stubs", "lib/Rivulet", "lib/Utf8", "lib/Serialization", "lib/Memory", "lib/GfxRenderer", "lib/JsonParser", "src/util"]
    sources = ["tools/casper_stability/regression.cpp", "lib/Rivulet/ChapterIr.cpp", "lib/Rivulet/HtmlToIr.cpp", "lib/Rivulet/IrTokenizer.cpp", "lib/Rivulet/PageLayouter.cpp", "lib/Rivulet/PageMap.cpp", "lib/Rivulet/LaidOutPage.cpp", "lib/Rivulet/FontLadder.cpp", "lib/Rivulet/RivuletEngine.cpp", "lib/Utf8/Utf8.cpp", "lib/GfxRenderer/FontCacheManager.cpp", "lib/JsonParser/StreamingJsonParser.cpp"]
    cmd = [os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-g", "-fno-exceptions", "-DCASPER_ALLOCATION_TESTING", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    cmd += ["-I"+str(root/p) for p in includes] + [str(root/p) for p in sources] + ["-o",str(exe)]
    subprocess.run(cmd,check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1:halt_on_error=1","UBSAN_OPTIONS":"halt_on_error=1"})
