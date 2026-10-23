#!/usr/bin/env python3
"""Host tests of the gen8 (Adreno 830) profile of the sheng GMU board code: generate build/erhai-gmu, build ShengGmu*.c
with it plus the AdrenoFirmware kit and package/ErhaiGmuHostTests.c, run it on the pinned gen80000_gmu.bin. Software
peers only; the tablet run is the evidence."""
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prepare', ROOT / 'scripts/prepare-erhai-gmu.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
out, _ = module.prepare()
kit, board = ROOT / 'kits/AdrenoFirmware', ROOT / 'kits/sheng'
sources = [board / 'ShengGmu.c', board / 'ShengGmuVotes.c', board / 'ShengGmuCommandDb.c', kit / 'AdrenoFirmware.c',
           kit / 'AdrenoHfi.c', kit / 'AdrenoHfiStartup.c', kit / 'AdrenoHfiVotes.c', board / 'ErhaiGmuHostTests.c']
test = out / 'test-erhai-gmu'
subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', out, '-I', kit, '-I', board, *sources, '-o', test],
               check=True, timeout=120)
subprocess.run([test, ROOT / '.cache/firmware/erhai-gpu/qcom/gen80000_gmu.bin'], check=True, timeout=60)
