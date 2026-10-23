#!/usr/bin/env python3
"""S6b host check: the Mesa 26.1 Turnip darwin backend (erhai's rebased patches) as an Adreno 830 over the host NativeAdreno stack.

The same run as check-turnip-darwin-host.py's smoke and plugin sequence (tests/hostsim: the real GPU context, VM, ring,
submit and user-client code over software SMMU/CP peers, an IOKit shim for the backend's calls), with the user client's
device info replaced by erhai's A830 facts (ErhaiGpuDeviceFacts: chip id 0x44050001, 12 MiB gmem at 64 MiB, UBWC 5 / bank
bit 16 / swizzle 2+3 / 8-channel macrotile). Turnip therefore picks its a8xx_gen1 device entry, compiles the smoke shaders
to A8XX IR3 and records A8XX command streams. The software CP retires every submission at once and executes nothing: this
proves the driver builds devices, pipelines and submissions for the A830 without failing, not any result.

Needs the host build of the 26.1 checkout: .cache/mesa-26.1 of UniXNU-erhai-ng configured into .cache/build-tu-host."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ERHAI = (ROOT / 'kits/sheng').resolve().parent
kit = ROOT / 'drivers/NativeAdreno'
sim = kit / 'tests/hostsim'
smmu = ROOT / 'kits/ArmSmmu4K'
out = ROOT / 'build/erhai-turnip-host'
out.mkdir(parents=True, exist_ok=True)
library = ERHAI / '.cache/build-tu-host/src/freedreno/vulkan/libvulkan_freedreno.dylib'
mesa = ERHAI / '.cache/mesa-26.1'
if not library.exists():
    sys.exit('Build the host Turnip of .cache/mesa-26.1 first (.cache/build-tu-host)')
run = lambda cmd, **kw: subprocess.run([str(c) for c in cmd], check=True, **kw)

includes = out / 'host-include'
# The fixture predates NativeAdrenoMemory's persistent imports (IODirection, kIOMemoryPersistent); both are only named on the host.
(includes / 'ErhaiFixture.h').parent.mkdir(parents=True, exist_ok=True)
(includes / 'ErhaiFixture.h').write_text('#pragma once\n#include "XnuMemoryFixture.h"\nusing IODirection=unsigned;\n'
                                         'constexpr unsigned kIOMemoryPersistent=0x10000;\n')
for name in ['IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h', 'IOKit/IOLib.h', 'IOKit/IOMemoryDescriptor.h',
             'kern/task.h']:
    path = includes / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "ErhaiFixture.h"\n')
objects = []
for stem in ['ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams', 'ArmSmmu4KMmio']:
    objects.append(out / (stem + '.o'))
    run(['xcrun', 'clang', '-std=c11', '-O1', '-fPIC', '-c', smmu / (stem + '.c'), '-o', objects[-1]])
objects.append(out / 'AdrenoPM4.o')
run(['xcrun', 'clang', '-std=c11', '-O1', '-fPIC', '-c', ROOT / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', objects[-1]])

# The host service with erhai's device info in place of the A740's.
source = (sim / 'host_gpu_service.cpp').read_text()
a740 = ('const NativeAdrenoAbiDeviceInfo info={0x43050a01u,3u<<20,0x1000000ull,0x1fffffffff000ull,0x20000000ull,0x60000000ull,\n'
        '    16,6,1,4,NativeAdrenoDeviceClientIova,0};')
if a740 not in source:
    sys.exit('host_gpu_service.cpp device info changed')
facts = ERHAI / 'package/ErhaiGpuHwInit.c'
gen = ROOT / 'build/erhai-gpu'
probe = out / 'facts.c'
probe.write_text('#include <stdio.h>\n#include "ErhaiGpuHwInit.h"\nint main(void){ErhaiGpuFacts f;ErhaiGpuDeviceFacts(&f);'
                 'printf("{0x%xu,0x%xu,0x%llxull,0x%llxull,0x20000000ull,0x60000000ull,\\n    %u,%u,%u,%u,NativeAdrenoDeviceClientIova,0}",'
                 'f.chip_id,f.gmem_bytes,(unsigned long long)f.gmem_base,(unsigned long long)f.uche_trap_base,f.highest_bank_bit,'
                 'f.ubwc_swizzle,f.macrotile_mode,f.ubwc_version);return 0;}\n')
run(['xcrun', 'clang', '-std=gnu11', '-I', ERHAI / 'package', '-I', gen, '-I', ROOT / 'kits/AdrenoPM4', probe, facts,
     ROOT / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', out / 'facts'])
a830 = subprocess.run([str(out / 'facts')], capture_output=True, text=True, check=True).stdout
if not a830.startswith('{0x44050001u,0xc00000u,0x4000000ull,'):
    sys.exit('Unexpected A830 facts: ' + a830)
(out / 'host_gpu_service.cpp').write_text(source.replace(a740, 'const NativeAdrenoAbiDeviceInfo info=' + a830 + ';'))
service = out / 'host_gpu_service.o'
flags = ['-std=c++23', '-O1', '-fPIC', '-I', includes, '-I', sim.parent, '-I', sim, '-I', kit, '-I', smmu, '-I', ROOT / 'kits/AdrenoPM4']
run(['xcrun', 'clang++', *flags, '-c', out / 'host_gpu_service.cpp', '-o', service])
core = []
for name in ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm', 'NativeAdrenoRing', 'NativeAdrenoSubmit',
             'NativeAdrenoUserClient', 'NativeAdrenoTimer', 'NativeAdrenoSmmuContext', 'NativeAdrenoGpuContext']:
    core.append(out / (name + '.o'))
    run(['xcrun', 'clang++', *flags, '-c', kit / (name + '.cpp'), '-o', core[-1]])
# XNU's proc_selfpid, which the user client's import path names; the host answers with its own pid.
extra = out / 'host_kernel_extra.cpp'
extra.write_text('#include <unistd.h>\nextern "C" int proc_selfpid(void) {return getpid();}\n')
run(['xcrun', 'clang++', '-std=c++17', '-O1', '-fPIC', '-c', extra, '-o', out / 'host_kernel_extra.o'])
core.append(out / 'host_kernel_extra.o')
shim_obj = out / 'host_iokit_shim.o'
run(['xcrun', 'clang++', '-std=c++17', '-O1', '-fPIC', '-c', sim / 'host_iokit_shim.cpp', '-o', shim_obj])
shim = out / 'libhostiokit.dylib'
run(['xcrun', 'clang++', '-dynamiclib', '-o', shim, service, shim_obj, *core, *objects, '-framework', 'IOKit', '-framework', 'CoreFoundation'])
smoke = out / 'vulkan_smoke'
run(['xcrun', 'clang', '-std=c11', '-O1', '-I', mesa / 'include', sim / 'vulkan_smoke.c', '-o', smoke, '-Wl,-rpath,' + str(out)])
env = dict(os.environ, DYLD_INSERT_LIBRARIES=str(shim), MESA_LOG='')
env.pop('TU_DARWIN_SERVICE', None)
def check(name, program):
    result = subprocess.run([str(program), str(library)], env=env, capture_output=True, text=True, timeout=300)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    if result.returncode or (name == 'smoke' and 'PASS' not in result.stdout):
        print(result.stderr[-3000:], end='')
        sys.exit('%s failed (exit %d); see %s' % (name, result.returncode, out / (name + '.log')))
    text = result.stdout + result.stderr
    if 'Adreno (TM) 830' not in text and 'FD830' not in text and '830' not in text:
        print('note: %s output does not name the device' % name)

check('smoke', smoke)
run([sys.executable, ROOT / 'scripts/gen-turnip-smoke-shaders.py'])
plugin_smoke = out / 'plugin_smoke'
run(['xcrun', 'clang++', '-std=c++17', '-O1', '-I', ROOT / 'drivers/NativeMetal', '-I', mesa / 'include', '-I', ROOT / 'build/turnip-smoke',
     sim / 'plugin_smoke.cpp', '-o', plugin_smoke])

check('plugin_smoke', plugin_smoke)
print('PASS: Mesa 26.1 Turnip (darwin backend) as an Adreno 830 built a device, pipelines (fill, compute, render pass) and '
      'submissions over the host kernel stack; the software CP executes nothing')
