#!/usr/bin/env python3
"""Compile and execute actual reader sources with deterministic host HAL/fonts."""
from pathlib import Path
import os, subprocess, tempfile
from concurrent.futures import ThreadPoolExecutor
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="casper-rc-") as d:
    exe = Path(d) / "regression"
    includes = ["tools/field_regression/stubs", "tools/casper_stability/stubs", "lib/Rivulet", "lib/Utf8", "lib/Serialization", "lib/Memory", "lib/GfxRenderer", "lib/JsonParser", "src/util"]
    sources = ["lib/Epub/Epub/css/CssParser.cpp", "tools/casper_stability/regression.cpp", "lib/Rivulet/ChapterIr.cpp", "lib/Rivulet/ReadinessCoordinator.cpp", "lib/Rivulet/HtmlToIr.cpp", "lib/Rivulet/IrTokenizer.cpp", "lib/Rivulet/PageLayouter.cpp", "lib/Rivulet/PageMap.cpp", "lib/Rivulet/LaidOutPage.cpp", "lib/Rivulet/FontLadder.cpp", "lib/Rivulet/RivuletEngine.cpp", "lib/Utf8/Utf8.cpp", "lib/GfxRenderer/FontCacheManager.cpp", "lib/JsonParser/StreamingJsonParser.cpp"]
    cmd = [os.environ.get("CXX", "g++"), "-std=c++20", os.environ.get("CASPER_TEST_OPT", "-O1"), "-g", "-fno-exceptions", "-DCASPER_ALLOCATION_TESTING", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    flags = cmd + ["-I"+str(root/p) for p in includes]
    def compile_one(item):
        i, source = item
        obj = Path(d) / (str(i)+".o")
        print("Compiling", source, flush=True)
        subprocess.run(flags + ["-c", str(root/source), "-o", str(obj)], check=True)
        return str(obj)
    with ThreadPoolExecutor(max_workers=2) as pool:
        objects=list(pool.map(compile_one,enumerate(sources)))
    subprocess.run(cmd + objects + ["-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1:halt_on_error=1","UBSAN_OPTIONS":"halt_on_error=1"})
