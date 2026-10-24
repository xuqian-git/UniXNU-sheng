#!/usr/bin/env python3
"""Fetch the UniXNU diagnostic log (package/BootLog.c) from the erhai log reader (package/linux-log/init-erhai): wait for its read-only
USB volume ERHAILOG, copy the log window and the export region off it, decode them with read-sheng-log.py's decoder, and eject the volume,
which sends the tablet back to fastboot (--no-eject keeps it).

usage: read-erhai-log.py [--out build/erhai-logs] [--wait 300] [--no-eject]
Output: DIR/<tag>-<boot id>/ as read-sheng-log.py writes it (header.txt, msgbuf.txt, slots.txt, latest.txt, export/, diag/), plus
reader/ with the reader's own reader.txt, insmod.txt and dmesg.txt, and pstore/ (the previous boot's console)."""
import argparse
import importlib.util
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VOLUME = Path('/Volumes/ERHAILOG')
EXPORT_BLOCKS = 65536   # below.bin: the export region under the window (NativeUfs kExportBlocks)

spec = importlib.util.spec_from_file_location('read_sheng_log', ROOT / 'scripts/read-sheng-log.py')
sheng = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sheng)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=ROOT / 'build/erhai-logs')
    ap.add_argument('--wait', type=int, default=300)
    ap.add_argument('--no-eject', action='store_true')
    a = ap.parse_args()
    end = time.time() + a.wait
    while not (VOLUME / 'reader.txt').exists():
        if time.time() > end:
            raise SystemExit(f'no {VOLUME} within {a.wait} s')
        time.sleep(1)
    time.sleep(1)
    print((VOLUME / 'reader.txt').read_text(errors='replace').rstrip())
    window = (VOLUME / 'window.bin').read_bytes()
    try:
        run = sheng.decode(window, a.out)
    except SystemExit as e:
        run = a.out / 'unreadable'
        run.mkdir(parents=True, exist_ok=True)
        print(e, file=sys.stderr)
    reader = run / 'reader'
    reader.mkdir(exist_ok=True)
    for name in ('reader.txt', 'insmod.txt', 'dmesg.txt'):
        if (VOLUME / name).exists():
            shutil.copyfile(VOLUME / name, reader / name)
    if (VOLUME / 'pstore').is_dir():
        shutil.copytree(VOLUME / 'pstore', reader / 'pstore', dirs_exist_ok=True)
    h = sheng.header(window)
    below = h.get('export_below')
    if h['magic'] == b'SHLOG001' and below and (h.get('export_bytes') or h.get('diag_bytes')):
        with open(VOLUME / 'below.bin', 'rb') as f:
            for first, size, name in ((h.get('export_first'), h.get('export_bytes'), 'export'), (h.get('diag_first'), h.get('diag_bytes'), 'diag')):
                if not size:
                    continue
                f.seek((EXPORT_BLOCKS - below + first) * sheng.BLOCK)
                sheng.unpack_export(f.read(size), run, name)
    if not a.no_eject:
        subprocess.run(['diskutil', 'eject', str(VOLUME)], check=True)
        print('ejected: the tablet returns to fastboot')
    print(run)


if __name__ == '__main__':
    main()
