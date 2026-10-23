#!/usr/bin/env python3
"""S6b: build Turnip (Mesa freedreno Vulkan, darwin backend) for erhai's Adreno 830 from the pin of scripts/data/erhai-mesa-inputs.json.

Prepares .cache/mesa-26.1 of this repository once (sparse clone of the pinned tag, commit and tree checked, the rebased patches of
patches/mesa-26.1 applied in order), refuses a checkout whose changes are not exactly those patches, then configures and builds
libvulkan_freedreno.dylib for --target host (.cache/build-tu-host, for check-erhai-turnip-host.py) or ios (.cache/build-tu-ios,
arm64 iOS 17). Python build modules come from the .cache/mesa-venv virtual environment; bison 3, glslang, glslc and spirv-val from
Homebrew. Nothing here runs a GPU, and nothing built is committed."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ERHAI = Path(__file__).resolve().parents[1]
if (ERHAI / 'kits/sheng').is_dir():   # run from the build tree (which also carries a copy of scripts/data): kits/sheng is this package
    ERHAI = (ERHAI / 'kits/sheng').resolve().parent
CACHE = ERHAI / '.cache'
pin = json.loads((ERHAI / 'scripts/data/erhai-mesa-inputs.json').read_text())
SOURCE = CACHE / 'mesa-26.1'
VENV = CACHE / 'mesa-venv'
UNIXNU = ERHAI.parent / 'UniXNU'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--target', choices=('host', 'ios'), default='ios')
parser.add_argument('--abi-dir', type=Path, default=UNIXNU / 'drivers/NativeAdreno', help='directory holding NativeAdrenoABI.h')
args = parser.parse_args()


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def git(*a):
    return subprocess.run(['git', '-C', str(SOURCE), *a], check=True, capture_output=True, text=True).stdout.strip()


if not SOURCE.exists():
    run(['git', 'clone', '-q', '--depth', '1', '--branch', pin['tag'], '--filter=blob:none', '--no-checkout', pin['repository'], SOURCE])
    git('sparse-checkout', 'set', '--no-cone', *pin['sparse'])
    git('checkout', '-q', '-b', 'erhai-26.1', pin['commit'])
    for patch in pin['patches']:
        run(['git', '-C', SOURCE, 'apply', ERHAI / patch])
if git('rev-parse', 'HEAD') != pin['commit'] or git('rev-parse', 'HEAD^{tree}') != pin['tree']:
    sys.exit('Mesa checkout is not the pinned ' + pin['tag'])
# The working tree must be exactly the pinned commit plus the recorded patches.
index = CACHE / 'mesa-26.1.verify-index'
index.unlink(missing_ok=True)
env = dict(os.environ, GIT_INDEX_FILE=str(index))
subprocess.run(['git', '-C', str(SOURCE), 'read-tree', pin['commit']], check=True, env=env)
subprocess.run(['git', '-C', str(SOURCE), 'add', '-A', '--', *(p.lstrip('/') for p in pin['sparse'])], check=True, env=env)
actual = subprocess.run(['git', '-C', str(SOURCE), 'write-tree'], check=True, env=env, capture_output=True, text=True).stdout.strip()
subprocess.run(['git', '-C', str(SOURCE), 'read-tree', pin['commit']], check=True, env=env)
for patch in pin['patches']:
    subprocess.run(['git', '-C', str(SOURCE), 'apply', '--cached', str(ERHAI / patch)], check=True, env=env)
expected = subprocess.run(['git', '-C', str(SOURCE), 'write-tree'], check=True, env=env, capture_output=True, text=True).stdout.strip()
index.unlink(missing_ok=True)
if actual != expected:
    sys.exit('Mesa checkout differs from the pinned tag plus patches/mesa-26.1 (%s != %s)' % (actual, expected))

if not (VENV / 'bin/meson').exists():
    run([sys.executable, '-m', 'venv', VENV])
    run([VENV / 'bin/pip', 'install', '-q', *pin['python']])
env = dict(os.environ)
env['PATH'] = os.pathsep.join([str(VENV / 'bin'), '/opt/homebrew/opt/bison/bin', '/opt/homebrew/bin', env['PATH']])
for tool in ('bison', 'glslangValidator', 'ninja'):
    if not subprocess.run(['which', tool], env=env, capture_output=True).returncode == 0:
        sys.exit(tool + ' is required (Homebrew)')

out = CACHE / ('build-tu-' + args.target)
options = ['-Dbuildtype=release', '-Db_ndebug=true', '-Dgallium-drivers=', '-Dvulkan-drivers=freedreno', '-Dfreedreno-kmds=darwin',
           '-Dtu-darwin-abi-dir=' + str(args.abi_dir.resolve()), '-Dplatforms=macos', '-Dglx=disabled', '-Degl=disabled',
           '-Dgbm=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=disabled', '-Dxmlconfig=disabled',
           '-Dshader-cache=disabled', '-Dtools=', '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dvulkan-layers=']
setup = [VENV / 'bin/meson', 'setup', out, SOURCE, '--wrap-mode=nodownload']
if args.target == 'ios':
    read = lambda cmd: subprocess.check_output(cmd, text=True).strip()
    sdk = read(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'])
    find = lambda tool: read(['xcrun', '--sdk', 'iphoneos', '--find', tool])
    flags = ['-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-fstack-protector-strong']
    cross = CACHE / 'ios.cross'
    cross.write_text(
        '[binaries]\nc = %r\ncpp = %r\nobjc = %r\nar = %r\nstrip = %r\npkg-config = \'/usr/bin/false\'\n'
        % (find('clang'), find('clang++'), find('clang'), find('ar'), find('strip')) +
        "[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n[built-in options]\n" +
        ''.join('%s = %r\n' % (k, flags) for k in ('c_args', 'cpp_args', 'objc_args', 'c_link_args', 'cpp_link_args',
                                                    'objc_link_args')))
    setup += ['--cross-file', cross]
if not (out / 'build.ninja').exists():
    run(setup + options, env=env)
run(['ninja', '-C', out, 'src/freedreno/vulkan/libvulkan_freedreno.dylib'], env=env)
library = out / 'src/freedreno/vulkan/libvulkan_freedreno.dylib'
print('PASS: Turnip (Mesa %s + patches/mesa-26.1, darwin backend) -> %s' % (pin['tag'], library))
