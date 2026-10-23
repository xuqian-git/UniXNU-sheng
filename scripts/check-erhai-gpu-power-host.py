#!/usr/bin/env python3
"""Host test for the erhai gated GPU power / SMMU observation sequence (kits/sheng/ErhaiGpuBoard.h) over a software register peer.

Run without the cross-compiler on PATH. A model test; nothing here touches hardware.
"""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').exists() else ROOT / 'package'
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'egb'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined',
                    '-I', str(PACKAGE), str(PACKAGE / 'ErhaiGpuBoardTests.c'), '-o', str(exe)], check=True)
    print(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip())
