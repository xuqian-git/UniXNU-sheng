#!/usr/bin/env python3
"""erhai boot image: gzip(BootShim + UEFI FD + payload) with the stock DTBs, and an empty ramdisk.

The OnePlus ABL replaces the ramdisk field during "fastboot boot", so the payload FAT volume rides in the kernel field
right after the firmware volume and BootShim (built with EMBEDDED_PAYLOAD_SIZE) hands it to UEFI. Ported from the
embed_payload mode of the old erhai unixnu bootimg. The image is meant for "fastboot boot" only and is verified by
unpacking it again; the output is deterministic.

  package-erhai-boot.py --uefi ../UEFI-erhai --dtb vendor_boot.dtb --payload out/erhai/payload/payload.img -o boot.img
"""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import zlib
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]


class Error(Exception):
    pass


def tool(name, hint=''):
    path = shutil.which(name)
    if not path:
        raise Error(f'{name} not found' + (f' ({hint})' if hint else ''))
    return path


def run(command, capture=False):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if result.returncode:
        raise Error(f'{command[0]} failed: {result.stderr.strip() or result.stdout.strip()}')
    return result.stdout


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


DEFAULTS = {'header_version': 0, 'base': 0, 'kernel_offset': 0x8000, 'tags_offset': 0x1e00000, 'pagesize': 4096}
MKBOOTIMG_FLAGS = ('base', 'kernel_offset', 'tags_offset', 'pagesize', 'header_version', 'ramdisk_offset', 'second_offset', 'cmdline')


def split_dtb(data):
    """Preserve concatenated stock DTBs so the bootloader can select its hardware variant."""
    trees = []
    offset = 0
    while offset < len(data):
        if len(data) - offset < 40 or data[offset:offset + 4] != b'\xd0\x0d\xfe\xed':
            raise Error('invalid compiled device tree')
        size = struct.unpack_from('>I', data, offset + 4)[0]
        if size < 40 or size > len(data) - offset:
            raise Error('invalid compiled device tree size')
        trees.append(data[offset:offset + size])
        offset += size
    if not trees:
        raise Error('empty compiled device tree')
    return trees


def compile_shim(uefi, base, size, requires_header, scratch, payload_size=0):
    clang, objcopy, nm = tool('clang'), tool('llvm-objcopy', 'brew install llvm'), tool('llvm-nm', 'brew install llvm')
    source = uefi / 'BootShim/BootShim.S'
    if not source.is_file():
        raise Error(f'{source} missing: --uefi must be a Mu-Silicium checkout')
    obj = scratch / 'shim.o'
    run([clang, '--target=aarch64-none-elf', '-c', f'-DFD_BASE={base}', f'-DFD_SIZE={size}',
         f'-DEMBEDDED_PAYLOAD_SIZE={payload_size}',
         f'-DREQUIRES_KERNEL_HEADER={int(requires_header)}', source, '-o', obj], capture=True)
    run([objcopy, '-O', 'binary', obj, scratch / 'shim.bin'], capture=True)
    shim = (scratch / 'shim.bin').read_bytes()
    symbols = run([nm, '-n', obj], capture=True)
    offset = next((int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' _Payload')), None)
    if offset != len(shim):
        raise Error('a literal pool or padding separates the _Payload label from the appended firmware volume')
    if requires_header and (shim[56:60] != b'ARM\x64' or struct.unpack_from('<QQ', shim, 8) != (base, len(shim) + size + payload_size)):
        raise Error('the ARM64 image header does not describe the shim, firmware and embedded payload')
    return shim


def patch_dtb(dtb, nodes, scratch):
    """Add device-tree nodes the stock bootloader expects (board.toml [[bootimg.dtb_node]])."""
    if not nodes:
        return dtb
    fdtput, fdtget = tool('fdtput', 'brew install dtc'), tool('fdtget', 'brew install dtc')
    work = scratch / 'device.dtb'
    work.write_bytes(dtb)
    for node in nodes:
        run([fdtput, '-c', work, node['path']], capture=True)
        for prop in node.get('props', []):
            values = prop['value'] if isinstance(prop['value'], list) else [prop['value']]
            run([fdtput, '-t', prop.get('type', 's'), work, node['path'], prop['name'], *[str(v) for v in values]], capture=True)
            got = run([fdtget, '-t', prop.get('type', 's'), work, node['path'], prop['name']], capture=True).split()
            kind = prop.get('type', 's')
            if kind in ('x', 'i', 'u') and [int(v, 16 if kind == 'x' else 10) for v in got] != [int(str(v), 0) for v in values]:
                raise Error(f'dtb node {node["path"]} property {prop["name"]} did not round-trip')
    return work.read_bytes()


def verify(path, combined, tree, ramdisk_path, image):
    """Unpack the image and compare every part with its input."""
    data = Path(path).read_bytes()
    if len(data) < 44 or data[:8] != b'ANDROID!':
        raise Error('invalid or truncated boot image header')
    version = struct.unpack_from('<I', data, 40)[0]
    if version != image['header_version'] or version not in range(5):
        raise Error('boot image header does not match the requested parameters')
    addresses = {}
    signature_size = 0
    if version >= 3:
        kernel_size, ramdisk_size, _, header_size = struct.unpack_from('<4I', data, 8)
        page = 4096
        if (header_size != (1584 if version == 4 else 1580) or len(data) < page or
                data[24:40] != bytes(16) or image['pagesize'] != page):
            raise Error('invalid v3/v4 boot image header')
        if version == 4:
            signature_size = struct.unpack_from('<I', data, 1580)[0]
    else:
        kernel_size, kernel_address, ramdisk_size, _, second_size, _, tags, page = struct.unpack_from('<8I', data, 8)
        if second_size or page != image['pagesize'] or page < (1632, 1648, 1660)[version]:
            raise Error('boot image header does not match the requested parameters')
        if kernel_address != image['base'] + image['kernel_offset'] or tags != image['base'] + image['tags_offset']:
            raise Error('boot image load addresses do not match the requested offsets')
        addresses = {'kernel_address': hex(kernel_address), 'tags_address': hex(tags)}
    start = page * (1 + -(-kernel_size // page))
    end = start + ramdisk_size
    if page + kernel_size > len(data) or end > len(data):
        raise Error('boot image payload is truncated')
    if version >= 3 and page * -(-end // page) + page * -(-signature_size // page) != len(data):
        raise Error('boot image signature or trailing layout does not match its header')
    kernel = data[page:page + kernel_size]
    unpacker = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        unpacked = unpacker.decompress(kernel)
    except zlib.error as error:
        raise Error(f'invalid boot image gzip: {error}') from None
    if unpacked != combined or not unpacker.eof or unpacker.unused_data != tree:
        raise Error('gzip, firmware and appended device tree do not round-trip')
    if data[start:start + ramdisk_size] != Path(ramdisk_path).read_bytes():
        raise Error('ramdisk bytes differ')
    return {'kernel_sha256': hashlib.sha256(kernel).hexdigest(), 'ramdisk_bytes': ramdisk_size,
            'header_version': version, **addresses}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--uefi', type=Path, required=True, help='the UEFI-erhai (Mu-Silicium) checkout')
    parser.add_argument('--fd', type=Path, help='firmware volume (default: board.toml [firmware] fd, DEBUG)')
    parser.add_argument('--dtb', type=Path, required=True, help='the stock compiled device tree(s)')
    parser.add_argument('--payload', type=Path, required=True, help='payload FAT volume (unixnu build payload)')
    parser.add_argument('--board', type=Path, default=DEVICE / 'board.toml')
    parser.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()
    board = tomllib.loads(args.board.read_text())
    fw = board.get('firmware', {})
    config = tomllib.loads((args.uefi / 'Resources/Configs' / f'{fw["config"]}.toml').read_text())
    image = dict(DEFAULTS)
    image.update(config.get('boot_image', {}))
    image.update(board.get('bootimg', {}).get('image', {}))
    base, size = config.get('uefi_fd', {}).get('base'), config.get('uefi_fd', {}).get('size')
    requires_header = config.get('boot_shim', {}).get('requires_kernel_header', True)
    fd = args.fd or args.uefi / fw['fd'].format(target='DEBUG')
    mkbootimg = args.uefi / 'Resources/Scripts/mkbootimg.py'
    for path, what in ((fd, 'firmware volume'), (args.dtb, 'device tree'), (args.payload, 'payload'), (mkbootimg, 'mkbootimg.py')):
        if not Path(path).is_file():
            raise Error(f'{what} not found: {path}')
    if base is None or size is None:
        raise Error('the Mu-Silicium device config has no [uefi_fd] base and size')
    firmware = Path(fd).read_bytes()
    if len(firmware) != size:
        raise Error(f'firmware volume is {len(firmware):#x} bytes, the device config says {size:#x}')
    payload = args.payload.read_bytes()
    if not payload or not requires_header:
        raise Error('the embedded payload needs a nonempty payload and an ARM64 image header')
    out = args.output
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='erhai-bootimg-') as scratch_name:
        scratch = Path(scratch_name)
        shim = compile_shim(args.uefi, base, size, requires_header, scratch, len(payload))
        tree = b''.join(patch_dtb(blob, board.get('bootimg', {}).get('dtb_node', []), scratch)
                        for blob in split_dtb(args.dtb.read_bytes()))
        combined = shim + firmware + payload
        kernel_path = scratch / 'Image.gz-dtb'
        kernel_path.write_bytes(gzip.compress(combined, mtime=0) + tree)
        ramdisk_path = scratch / 'ramdisk'
        ramdisk_path.write_bytes(b'')
        command = [sys.executable, mkbootimg, '--kernel', kernel_path, '--ramdisk', ramdisk_path, '-o', out]
        for key in MKBOOTIMG_FLAGS:
            if key in image:
                command += [f'--{key}', str(image[key])]
        run(command, capture=True)
        report = verify(out, combined, tree, ramdisk_path, image)
    report.update(image_sha256=sha256_file(out), image_bytes=out.stat().st_size, fd_sha256=sha256_file(fd),
                  dtb_sha256=sha256_file(args.dtb), payload_sha256=sha256_file(args.payload), shim_bytes=len(shim),
                  embedded_payload_bytes=len(payload))
    (out.parent / 'boot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'erhai boot image: {out} ({out.stat().st_size} bytes, payload {len(payload)} bytes embedded)')


if __name__ == '__main__':
    try:
        main()
    except Error as error:
        sys.exit(f'package-erhai-boot: {error}')
