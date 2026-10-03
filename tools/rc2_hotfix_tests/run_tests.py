#!/usr/bin/env python3
"""Actual HAL implementation with mocked SdFat/RTOS/clock; no device claim."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
suite=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="casper-hal-") as d:
    exe=Path(d)/"hal-lifecycle"
    cmd=[os.environ.get("CXX","g++"),"-std=c++20","-g","-O1","-fno-exceptions","-fsanitize=address,undefined","-fno-omit-frame-pointer","-no-pie","-I"+str(suite/"stubs"),"-I"+str(root/"lib/hal"),"-I"+str(root/"lib/Rivulet"),"-I"+str(root/"lib/Memory"),str(suite/"hal_lifecycle.cpp"),str(root/"lib/hal/HalStorage.cpp"),"-pthread","-o",str(exe)]
    subprocess.run(cmd,check=True)
    env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1:halt_on_error=1","UBSAN_OPTIONS":"halt_on_error=1"}
    subprocess.run([str(exe)],check=True,env=env)
    result=subprocess.run([str(exe),"badread"],capture_output=True,text=True,env=env)
    assert result.returncode!=0 and 'impl != nullptr' in result.stderr, result.stderr
    print("Invalid read still asserts: I/O misuse detection was not disabled.")

with tempfile.TemporaryDirectory(prefix="casper-cover-") as d:
    exe=Path(d)/"cover-retry"
    subprocess.run([os.environ.get("CXX","g++"),"-std=c++20","-g","-O1","-fno-exceptions","-fsanitize=address,undefined","-fno-omit-frame-pointer","-no-pie","-I"+str(root/"src/util"),str(suite/"cover_retry.cpp"),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env=env)
