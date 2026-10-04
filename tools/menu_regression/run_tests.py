#!/usr/bin/env python3
"""Production policy headers, not a physical panel or input-latency benchmark."""
import os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='casper-menu-') as tmp:
    exe=Path(tmp)/'policy'
    subprocess.run(['g++','-std=c++20','-O0','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
        '-I'+str(root/'tools/menu_regression/stubs'),'-I'+str(root/'src'),str(root/'tools/menu_regression/policy.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
