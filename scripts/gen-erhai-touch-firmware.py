#!/usr/bin/env python3
"""Emit build/native-touch/ErhaiTouchFirmware.c from the NT36536 firmware kept outside Git (.cache/firmware/touch).

The erhai touch controller (in-cell NT36536) loses its RAM image with the panel, so the host downloads it at every boot. The image is
the tablet's own /odm/firmware/tp/24924/FW_NT36536_HUAXING_FAE.img (identical to the body of FW_NT36536_HUAXING.img and to the live
DT's firmware-data-0; docs/erhai-touch-facts.md section 5), pinned by hash. The generated source embeds vendor firmware bytes, so it
lives only under build/ and is never committed. With no (or a different) image it emits an empty object and the touch probe stops
before the download stage (erhai-touch=4 reports e-2).
"""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / '.cache/firmware/touch/FW_NT36536_HUAXING_FAE.img'
SHA256 = '3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b'
OUT = ROOT / 'build/native-touch'


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    data = path.read_bytes() if path.is_file() else b''
    if data and hashlib.sha256(data).hexdigest() != SHA256:
        print(f'WARNING: {path} is not the pinned NT36536 image (sha256 {SHA256[:16]}); leaving it out', file=sys.stderr)
        data = b''
    if not data:
        print(f'WARNING: no erhai touch firmware at {path}: the image will have no touch.', file=sys.stderr)
    OUT.mkdir(parents=True, exist_ok=True)
    lines = ['/* Generated; contains vendor firmware, do not commit. */', '#include <stdint.h>']
    lines.append(f'const uint32_t erhai_touch_firmware_size = {len(data)};')
    body = ','.join(str(b) for b in data) if data else '0'
    lines.append(f'const uint8_t erhai_touch_firmware[{max(len(data), 1)}] = {{{body}}};')
    (OUT / 'ErhaiTouchFirmware.c').write_text('\n'.join(lines) + '\n')
    print(f'erhai touch firmware object: {len(data)} bytes sha256 {hashlib.sha256(data).hexdigest()[:16] if data else "-"}')


if __name__ == '__main__':
    main()
