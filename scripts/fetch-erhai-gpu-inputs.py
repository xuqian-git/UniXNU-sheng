#!/usr/bin/env python3
"""Fetch the SHA-pinned extra sources of the erhai Adreno 830 GMU (scripts/data/erhai-gpu-inputs.json) and check the local
OEM GMU firmware against its pin. Nothing is executed; the firmware is never downloaded."""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
pins = json.loads((ROOT / 'scripts/data/erhai-gpu-inputs.json').read_text())


def fetch():
    total = 0
    for name, expected in pins['files'].items():
        path = ROOT / '.cache/erhai-gpu-source' / name
        if path.exists():
            data = path.read_bytes()
        else:
            request = urllib.request.Request(expected['url'], headers={'User-Agent': 'erhai-offline-adreno-audit'})
            with urllib.request.urlopen(request, timeout=30) as response:
                data = response.read()
        if hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise SystemExit('Pinned input hash differs: ' + name)
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        total += len(data)
    for name, expected in pins['firmware'].items():
        path = ROOT / '.cache/firmware/erhai-gpu' / name
        if not path.exists():
            raise SystemExit('Missing local firmware ' + str(path) + ' (copy it from the pinned OTA, see the inputs file)')
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected['sha256'] or len(data) != expected['bytes']:
            raise SystemExit('Local firmware hash/size differs: ' + name)
    return total


if __name__ == '__main__':
    print(f'PASS: {len(pins["files"])} pinned erhai GPU sources ({fetch()} bytes) and the pinned local GMU firmware')
