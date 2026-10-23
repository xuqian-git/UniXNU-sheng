#!/usr/bin/env python3
"""S6c: build the owner Adreno Metal plugin (NativeAdrenoMetal.bundle) for erhai's Adreno 830 with the upstream builder.

UniXNU's build-adreno-metal-plugin.py takes Turnip, the Vulkan headers and the host simulation from fixed places in the build tree
(.cache/mesa-work, build/mesa-tu-ios, build/mesa-tu, build/turnip-darwin-host). Here they are the Mesa 26.1 builds of this repository
(build-erhai-turnip.py) and the A830 host simulation (check-erhai-turnip-host.py), linked into the tree; a real directory already at one
of those places is refused, never replaced. The upstream builder then compiles the smoke shaders, records Turnip's first A830 submission
for the prefix replay, cross-builds the metal2vulkan bridge from its pin (.cache/gpu-reference/metal2vulkan of the tree), links the
plugin and the probe daemon and signs them ad hoc with 4 KiB pages. Output (ignored, never committed): build/adreno-metal-plugin of the
tree, with trust.json for make-owner-trust-bundle.py."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ERHAI = Path(__file__).resolve().parents[1]
if (ERHAI / 'kits/sheng').is_dir():   # run from the build tree (which also carries a copy of scripts/data): kits/sheng is this package
    ERHAI = (ERHAI / 'kits/sheng').resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree', type=Path, default=ERHAI.parent / 'UniXNU/out/erhai/tree')
args = parser.parse_args()
tree = args.tree.resolve()
if (tree / 'kits/sheng').resolve().parent != ERHAI:
    sys.exit('%s is not a build tree of %s' % (tree, ERHAI))
run = lambda cmd, **kw: subprocess.run([str(c) for c in cmd], check=True, **kw)

# The erhai scripts run from the tree, as the board build places them.
for name in ['build-erhai-turnip.py', 'check-erhai-turnip-host.py']:
    shutil.copyfile(ERHAI / 'scripts' / name, tree / 'scripts' / name)
run([sys.executable, tree / 'scripts/build-erhai-turnip.py', '--target', 'host'])
run([sys.executable, tree / 'scripts/build-erhai-turnip.py', '--target', 'ios'])
run([sys.executable, tree / 'scripts/check-erhai-turnip-host.py'])

links = {tree / '.cache/mesa-work': ERHAI / '.cache/mesa-26.1', tree / 'build/mesa-tu-ios': ERHAI / '.cache/build-tu-ios',
         tree / 'build/mesa-tu': ERHAI / '.cache/build-tu-host', tree / 'build/turnip-darwin-host': tree / 'build/erhai-turnip-host'}
for link, target in links.items():
    if link.exists() and not link.is_symlink():
        sys.exit('%s exists and is not a link; move it away first' % link)
    if link.is_symlink():
        link.unlink()
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(target)

# The bridge builder compares `git diff` with its recorded patches, whose index lines carry 7-digit hashes; a full clone abbreviates
# longer, so the translator clone is held at 7.
translator = tree / '.cache/gpu-reference/metal2vulkan'
if translator.is_dir():
    run(['git', '-C', translator, 'config', 'core.abbrev', '7'])

# The upstream builder and the scripts it starts run as python3: the interpreter running this script comes first.
env = dict(os.environ, PATH=os.pathsep.join([str(Path(sys.executable).parent), os.environ['PATH']]))
run([sys.executable, tree / 'scripts/build-adreno-metal-plugin.py'], env=env)
print('PASS: NativeAdrenoMetal.bundle for the Adreno 830 (Mesa 26.1 Turnip) -> %s' % (tree / 'build/adreno-metal-plugin'))
