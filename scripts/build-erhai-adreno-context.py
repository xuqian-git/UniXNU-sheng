#!/usr/bin/env python3
"""Archive the part of NativeAdreno that builds an adreno-SMMU context for erhai: the GMU context, its payload allocator, the
VM and SMMU table memory, and the ArmSmmu4K kit. Nothing generation-specific (GMU firmware, HFI votes, CP, PM4) is included,
so no sheng firmware or generated GMU source is needed. ErhaiGpuPower.cpp (erhai-gpu-power=4) calls
NativeAdrenoGmuContextBringup from it; build-erhai-adreno-stubs.py skips every symbol this archive defines, so run it first.
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
out = ROOT / 'build/erhai-adreno-context'
out.mkdir(parents=True, exist_ok=True)
sources = sorted(p for p in (ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
sources += [ROOT / 'drivers/NativeAdreno' / (name + '.cpp') for name in
            ('NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory', 'NativeAdrenoSmmuContext', 'NativeAdrenoSmmuMemory',
             'NativeAdrenoVm', 'NativeAdrenoMemory', 'NativeAdrenoCpu')]
objects = []
for source in sources:
    recipe = c_recipe if source.suffix == '.c' else cxx_recipe
    obj = out / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
    command, i = [], 0
    while i < len(recipe['arguments']):
        value = recipe['arguments'][i]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and recipe['arguments'][i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += ['-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'drivers/NativeAdreno'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(str(source) + ':\n' + result.stderr[-2500:])
    objects.append(obj)
archive = out / 'libErhaiAdrenoContext.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
nm = lambda *flags: {line.split()[-1] for line in subprocess.run(['xcrun', 'nm', '-g', *flags, archive], capture_output=True, text=True,
                                                                  check=True).stdout.splitlines() if line.strip() and not line.endswith(':')}
defined, undefined = nm('--defined-only'), nm('-u')
# Every NativeAdreno/ArmSmmu4K symbol the archive itself needs must be inside it: a stub would turn a call into "return 0".
open_refs = sorted(s for s in undefined - defined if 'ArmSmmu4K' in s or 'NativeAdreno' in s or 'native_adreno' in s)
if open_refs:
    sys.exit('erhai Adreno context archive is not closed: ' + ', '.join(open_refs))
for symbol in ('_NativeAdrenoGmuContextBringup', '_NativeAdrenoGmuContextPayloads', '_ArmSmmu4KBindMmio'):
    if symbol not in defined:
        sys.exit('required definition absent: ' + symbol)
print(f'PASS: erhai Adreno context archive, {len(objects)} objects -> {archive}')
