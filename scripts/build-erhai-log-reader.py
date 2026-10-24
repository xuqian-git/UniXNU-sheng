#!/usr/bin/env python3
"""Build the fastboot-boot-only erhai log reader: the stock GKI kernel of the tablet's boot partition (kernel reused byte for byte) in a
header version 4 boot image whose ramdisk holds busybox, package/linux-log/init-erhai, rebootbl-erhai and etc/reader.load. ABL loads the
tablet's own vendor_boot in front of it, so the vendor modules come from the device; reader.load names the ones the reader loads, in the
recovery order with every module's dependencies ahead of it: the first-stage set (UFS, PMIC, reboot reason), ramoops and the USB stack with its EUD, usbss and glink suppliers, with their
dependencies, minus the oplus DFR monitors. The whole recovery set (minus the DFR monitors) kept restarting the tablet within seconds.
ABL takes the generic ramdisk from init_boot, not from a fastboot-booted image (a ramdisk here alone never ran: Android's init did and
restarted into recovery), so --init-boot writes the same ramdisk as an init_boot image for init_boot_b.
Nothing on UFS is mounted or written. Inputs and output stay outside Git.

usage: build-erhai-log-reader.py --boot boot_b.img --vendor-boot vendor_boot_b.img --busybox busybox --out reader.img"""
import argparse
import gzip
import hashlib
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INIT = ROOT / 'package/linux-log/init-erhai'
PAGE = 4096
HEADER_V4 = 1584
USB = ('dwc3-msm', 'phy-msm-snps-eusb2', 'phy-msm-m31-eusb2', 'phy-msm-ssusb-qmp', 'repeater-qti-pmic-eusb2', 'eud', 'wcd_usbss_i2c',
       'qti_pmic_glink', 'ucsi_qti_glink',
       'qcom_dynamic_ramoops')   # not USB: the ramoops region, so the volume carries the previous boot's console (pstore)
SKIP = ('oplus_bsp_dfr_', 'hung_task_enh', 'mtk_wdt')


def vendor_modules(image):
    """modules.load, modules.load.recovery and modules.dep of a header version 4 vendor_boot (one lz4 legacy or gzip ramdisk)."""
    v = image.read_bytes()
    if v[:8] != b'VNDRBOOT':
        raise SystemExit('not a vendor_boot image')
    page, _, _, size = struct.unpack_from('<4I', v, 12)
    (header_size,) = struct.unpack_from('<I', v, 28 + 2048 + 4 + 16)
    start = -(-header_size // page) * page
    blob = v[start:start + size]
    raw = subprocess.run(['lz4', '-dc'], input=blob, capture_output=True, check=True).stdout if blob[:4] == b'\x02\x21\x4c\x18' else gzip.decompress(blob)
    files, at = {}, 0
    while raw[at:at + 6] == b'070701':
        f = [int(raw[at + 6 + 8 * i:at + 14 + 8 * i], 16) for i in range(13)]
        name = raw[at + 110:at + 110 + f[11] - 1].decode()
        at = -(-(at + 110 + f[11]) // 4) * 4
        if name in ('lib/modules/modules.load', 'lib/modules/modules.load.recovery', 'lib/modules/modules.dep'):
            files[name.rsplit('/', 1)[1]] = raw[at:at + f[6]].decode()
        at = -(-(at + f[6]) // 4) * 4
    return files


def reader_load(files):
    dep = {}
    for line in files['modules.dep'].splitlines():
        mod, _, needs = line.partition(':')
        dep[mod.rsplit('/', 1)[1]] = [n.rsplit('/', 1)[1] for n in needs.split()]
    order = files['modules.load.recovery'].split()
    missing = [m + '.ko' for m in USB if m + '.ko' not in order]
    if missing:
        raise SystemExit(f'vendor_boot lacks {missing}')
    want, todo = set(), files['modules.load'].split() + [m + '.ko' for m in USB]
    while todo:
        m = todo.pop()
        if m in want or m.startswith(SKIP):
            continue
        want.add(m)
        todo += dep.get(m, [])
    # Dependencies first: the recovery list assumes modprobe, and plain insmod in its order left glink and rproc with unknown symbols.
    out, seen = [], set()

    def emit(m):
        if m in seen:
            return
        seen.add(m)
        for d in dep.get(m, []):
            emit(d)
        out.append(m)

    for m in order:
        if m in want:
            emit(m)
    return out


def cpio(entries):
    """newc archive; entries are (path, mode, data) with directories first."""
    out = bytearray()
    for ino, (name, mode, data) in enumerate(entries + [('TRAILER!!!', 0, b'')], 1):
        path = name.encode() + b'\0'
        out += b'070701' + b''.join(b'%08x' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(path), 0)) + path
        out += b'\0' * (-len(out) % 4) + data
        out += b'\0' * (-len(out) % 4)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    for name in ('boot', 'vendor-boot', 'busybox', 'out'):
        ap.add_argument('--' + name, required=True, type=Path)
    ap.add_argument('--init-boot', type=Path, help='also write the ramdisk as an init_boot image (ABL takes the ramdisk from init_boot)')
    ap.add_argument('--splashcon', type=Path, help='package/linux-log/splashcon built for this kernel (the screen console)')
    ap.add_argument('--cmdline', default='', help='boot image command line, e.g. reader.pause=30')
    ap.add_argument('--init', type=Path, default=INIT, help='another /init, for a bring-up probe')
    a = ap.parse_args()
    boot = a.boot.read_bytes()
    if boot[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from('<4I', boot, 8)
    (version,) = struct.unpack_from('<I', boot, 40)
    if version != 4 or header_size != HEADER_V4:
        raise SystemExit('expected the stock header version 4 boot image')
    kernel = boot[PAGE:PAGE + kernel_size]
    if kernel[0x38:0x3c] != b'ARM\x64':
        raise SystemExit('the boot image kernel is not an arm64 Image')
    # /init replaces the generic ramdisk's; lib/modules stays the vendor ramdisk's.
    load = reader_load(vendor_modules(a.vendor_boot))
    dirs = ['bin', 'dev', 'etc', 'proc', 'sys', 'run', 'tmp']
    entries = [(d, 0o40755, b'') for d in dirs]
    entries += [('dev/console', 0o20600, b''), ('init', 0o100755, a.init.read_bytes()),
                ('bin/busybox', 0o100755, a.busybox.read_bytes()), ('bin/rebootbl', 0o100755, (ROOT / 'package/linux-log/rebootbl-erhai').read_bytes()),
                ('etc/reader.load', 0o100644, ('\n'.join(load) + '\n').encode())]
    if a.splashcon:
        entries = entries[:len(dirs)] + [('lib', 0o40755, b''), ('lib/reader', 0o40755, b'')] + entries[len(dirs):]
        entries.append(('lib/reader/splashcon.ko', 0o100644, a.splashcon.read_bytes()))
    # lz4 legacy, as the generic ramdisk: a gzip ramdisk behind the vendor one never unpacked on this GKI kernel (no /init, a restart
    # into recovery about 10 s after fastboot boot).
    ramdisk = subprocess.run(['lz4', '-l', '-12', '--favor-decSpeed', '-c'], input=cpio(entries), capture_output=True, check=True).stdout
    pad = lambda b: b + b'\0' * (-len(b) % PAGE)
    header = bytearray(PAGE)
    header[:8] = b'ANDROID!'
    struct.pack_into('<4I', header, 8, len(kernel), len(ramdisk), os_version, HEADER_V4)
    struct.pack_into('<I', header, 40, 4)
    # header[44:1580] is the command line, in front of the vendor_boot line; signature_size 0.
    header[44:44 + len(a.cmdline)] = a.cmdline.encode()
    image = bytes(header) + pad(kernel) + pad(ramdisk)
    a.out.write_bytes(image)
    if a.init_boot:
        header = bytearray(PAGE)
        header[:8] = b'ANDROID!'
        struct.pack_into('<4I', header, 8, 0, len(ramdisk), os_version, HEADER_V4)
        struct.pack_into('<I', header, 40, 4)
        a.init_boot.write_bytes(bytes(header) + pad(ramdisk))
        print(f'BUILT {a.init_boot} (init_boot, {PAGE + len(pad(ramdisk))} bytes)')
    print(f'BUILT {a.out} ({len(image)} bytes) sha256 {hashlib.sha256(image).hexdigest()} ramdisk {len(ramdisk)} modules {len(load)}')


if __name__ == '__main__':
    main()
