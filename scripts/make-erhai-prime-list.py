#!/usr/bin/env python3
"""S6d: the render server's shader-priming list for erhai's target image (build/prime-list.txt of the build tree).

Every .metallib of iPadOS 21H461, one absolute path per line as the tablet sees it: the OS root image (component "OS") at /, the
SystemOS cryptex (component "Cryptex1,SystemOS") at /System/Cryptexes/OS. Both images of the IPSW cache (out/ipsw/21H461, from the
board build) are attached read-only and without browsing in a temporary directory, listed and detached again. The plugin builder embeds
the list in the executable (__DATA,__primelist), and make-gpu-addon.py --prime-list carries it beside the bundle (PrimeShaders.h).
Generated, never committed: the paths name Apple files, not their content."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ERHAI = Path(__file__).resolve().parents[1]
if (ERHAI / 'kits/sheng').is_dir():   # run from the build tree (which also carries a copy of scripts/data): kits/sheng is this package
    ERHAI = (ERHAI / 'kits/sheng').resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ipsw-dir', type=Path, default=ERHAI.parent / 'UniXNU/out/ipsw/21H461')
parser.add_argument('--output', type=Path, default=ERHAI.parent / 'UniXNU/out/erhai/tree/build/prime-list.txt')
args = parser.parse_args()
images = {}
for meta in args.ipsw_dir.glob('*.dmg.json'):
    info = json.loads(meta.read_text())
    images[info['component']] = args.ipsw_dir / info['member']
prefixes = {'OS': '', 'Cryptex1,SystemOS': '/System/Cryptexes/OS'}
if set(prefixes) - set(images):
    sys.exit('%s lacks the OS or SystemOS image (run the board build first)' % args.ipsw_dir)

paths = []
with tempfile.TemporaryDirectory() as temp:
    for component, prefix in prefixes.items():
        mount = Path(temp) / component.replace(',', '-')
        mount.mkdir()
        subprocess.run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-noverify', '-mountpoint', str(mount), str(images[component])],
                       check=True, capture_output=True)
        try:
            for root, _, files in os.walk(mount):
                for name in files:
                    full = Path(root) / name
                    if name.endswith('.metallib') and full.is_file() and not full.is_symlink():
                        paths.append(prefix + '/' + str(full.relative_to(mount)))
        finally:
            subprocess.run(['hdiutil', 'detach', str(mount)], check=True, capture_output=True)
paths.sort()
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(''.join(p + '\n' for p in paths))
print('PASS: %d .metallib paths of %s -> %s' % (len(paths), args.ipsw_dir.name, args.output))
