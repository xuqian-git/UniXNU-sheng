#!/usr/bin/env python3
"""Archive the part of NativeAdreno that builds an adreno-SMMU context for erhai: the GMU context, its payload allocator, the
VM and SMMU table memory, and the ArmSmmu4K kit. ErhaiGpuPower.cpp (erhai-gpu-power=4) calls NativeAdrenoGmuContextBringup
from it. For the GMU (erhai-gpu-power>=5) it also holds the gen8 profile of the sheng GMU board code (ShengGmu.c,
ShengGmuVotes.c, ShengGmuCommandDb.c built with the build/erhai-gmu headers of prepare-erhai-gmu.py), the AdrenoFirmware
kit and, for the cold start (erhai-gpu-power=7), gen80000_gmu.bin as erhai_gmu_firmware_image: assembled from the ignored
local copy only if it matches the pin in scripts/data/erhai-gpu-inputs.json, so the bytes stay out of Git. For the CP start
(erhai-gpu-power=9) it adds the GPU's own context, ring and CP init of NativeAdreno built as its gen8 (A8XX) profile, the
AdrenoPM4 kit, the erhai hw_init and zap loader (ErhaiGpuHwInit.c with build/erhai-gpu of prepare-erhai-gpu.py,
ErhaiGpuZap.c), and gen80000_sqe.fw / gen80000_zap.mbn as erhai_gpu_sqe_image / erhai_gpu_zap_image, pin-checked the same
way. build-erhai-adreno-stubs.py skips every symbol this archive defines, so run it first.
"""
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
out = ROOT / 'build/erhai-adreno-context'
out.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('prepare', ROOT / 'scripts/prepare-erhai-gmu.py')
prepare = importlib.util.module_from_spec(spec); spec.loader.exec_module(prepare)
gmu_headers, _ = prepare.prepare()
spec = importlib.util.spec_from_file_location('prepare_gpu', ROOT / 'scripts/prepare-erhai-gpu.py')
prepare_gpu = importlib.util.module_from_spec(spec); spec.loader.exec_module(prepare_gpu)
gpu_headers = prepare_gpu.prepare()
sources = sorted(p for p in (ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
sources += [ROOT / 'kits/AdrenoFirmware' / (name + '.c') for name in ('AdrenoFirmware', 'AdrenoHfi', 'AdrenoHfiStartup', 'AdrenoHfiVotes')]
sources += [ROOT / 'kits/AdrenoPM4/AdrenoPM4.c']
sources += [ROOT / 'kits/sheng' / (name + '.c') for name in ('ShengGmu', 'ShengGmuVotes', 'ShengGmuCommandDb', 'ErhaiGpuHwInit',
                                                             'ErhaiGpuZap', 'ErhaiGpuGlobals')]
sources += [ROOT / 'drivers/NativeAdreno' / (name + '.cpp') for name in
            ('NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory', 'NativeAdrenoSmmuContext', 'NativeAdrenoSmmuMemory',
             'NativeAdrenoVm', 'NativeAdrenoMemory', 'NativeAdrenoCpu', 'NativeAdrenoGpuContext', 'NativeAdrenoRing',
             'NativeAdrenoCpInit')]
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
    command += ['-DNATIVE_ADRENO_GEN8=1', '-I', str(gmu_headers), '-I', str(gpu_headers), '-I', str(ROOT / 'kits/AdrenoPM4'),
                '-I', str(ROOT / 'kits/AdrenoFirmware'), '-I', str(ROOT / 'kits/sheng'),
                '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'drivers/NativeAdreno'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(str(source) + ':\n' + result.stderr[-2500:])
    objects.append(obj)
import hashlib
firmware_pins = json.loads((ROOT / 'scripts/data/erhai-gpu-inputs.json').read_text())['firmware']
assembly = ['.section __DATA_CONST,__const']
for name, label in (('gen80000_gmu.bin', '_erhai_gmu_firmware_image'), ('gen80000_sqe.fw', '_erhai_gpu_sqe_image'),
                    ('gen80000_zap.mbn', '_erhai_gpu_zap_image')):
    pin = firmware_pins['qcom/' + name]
    firmware = ROOT / '.cache/firmware/erhai-gpu/qcom' / name
    blob = firmware.read_bytes() if firmware.is_file() else b''
    if len(blob) != pin['bytes'] or hashlib.sha256(blob).hexdigest() != pin['sha256']:
        sys.exit(f'firmware qcom/{name} is missing or does not match its pin')
    assembly += ['.p2align 12', '.globl ' + label, label + ':', '.incbin ' + json.dumps(str(firmware.resolve())), label + '_end:',
                 '.p2align 3', '.globl ' + label + '_size', label + '_size:', '.quad ' + label + '_end-' + label]
(out / 'firmware.S').write_text('\n'.join(assembly) + '\n')
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Wno-incompatible-sysroot', '-c', out / 'firmware.S',
                '-o', out / 'firmware.o'], check=True)
objects.append(out / 'firmware.o')
archive = out / 'libErhaiAdrenoContext.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
nm = lambda *flags: {line.split()[-1] for line in subprocess.run(['xcrun', 'nm', '-g', *flags, archive], capture_output=True, text=True,
                                                                  check=True).stdout.splitlines() if line.strip() and not line.endswith(':')}
defined, undefined = nm('--defined-only'), nm('-u')
# Every NativeAdreno/ArmSmmu4K symbol the archive itself needs must be inside it: a stub would turn a call into "return 0".
open_refs = sorted(s for s in undefined - defined if any(n in s for n in ('ArmSmmu4K', 'NativeAdreno', 'native_adreno', 'ShengGmu', 'Adreno')))
if open_refs:
    sys.exit('erhai Adreno context archive is not closed: ' + ', '.join(open_refs))
for symbol in ('_NativeAdrenoGmuContextBringup', '_NativeAdrenoGmuContextPayloads', '_ArmSmmu4KBindMmio', '_ShengGmuColdStart',
               '_ShengGmuBindMmio', '_ShengGmuBuildStartup', '_ShengGmuCommandDbInputs', '_AdrenoHfiSend',
               '_erhai_gmu_firmware_image', '_erhai_gmu_firmware_image_size', '_NativeAdrenoGpuContextBringup',
               '_NativeAdrenoGpuContextBuffers', '_ErhaiGpuHwInitPre', '_ErhaiGpuHwInitStartCp', '_ErhaiGpuHwInitPost',
               '_ErhaiGpuSecureModePacket', '_ErhaiGpuHwSnapshot', '_ErhaiZapLoad', '_AdrenoPM4Type7', '_erhai_gpu_sqe_image',
               '_erhai_gpu_sqe_image_size', '_erhai_gpu_zap_image', '_erhai_gpu_zap_image_size'):
    if symbol not in defined:
        sys.exit('required definition absent: ' + symbol)
print(f'PASS: erhai Adreno context archive, {len(objects)} objects -> {archive}')
