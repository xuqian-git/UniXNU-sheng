#!/usr/bin/env bash
# One unattended erhai test run: boot a test image, wait for the tablet to restart into fastboot by itself (the image's
# sheng-autoreset=<seconds>, or the hypervisor watchdog a minute after a panic), boot the log reader (its ramdisk goes to init_boot_b,
# where ABL takes the generic ramdisk from) and fetch the log with read-erhai-log.py, whose eject sends the tablet back to fastboot.
# If the tablet does not come back within the limit it says so and waits for a manual fastboot entry (Power + Volume down).
# usage: [SERIAL=<serial>] [READER_DIR=~/.cache/erhai-log-reader] run-erhai-unattended.sh IMAGE [wait_seconds=300]
# The test image needs sheng-log=1 (board.toml) and, for the run, --kernel-arg sheng-autoreset=120 --kernel-arg sheng-diag-export=110.
# READER_DIR holds erhai-log-reader.img and erhai-log-reader-init_boot.img (scripts/build-erhai-log-reader.py --init-boot).
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMG=${1:?image}
LIMIT=${2:-300}
READER_DIR=${READER_DIR:-$HOME/.cache/erhai-log-reader}
READER=$READER_DIR/erhai-log-reader.img
READER_INIT_BOOT=$READER_DIR/erhai-log-reader-init_boot.img
PY=${PY:-python3}
for f in "$IMG" "$READER" "$READER_INIT_BOOT"; do [ -f "$f" ] || { echo "missing $f"; exit 1; }; done
"$PY" -c 'import sys; sys.exit(sys.version_info < (3, 11))' || { echo "read-erhai-log.py needs Python 3.11 or newer (set PY)"; exit 1; }
present() { fastboot devices 2>/dev/null | grep -q "^${SERIAL:-}"; }
until present; do sleep 1; done
SERIAL=${SERIAL:-$(fastboot devices | awk 'NR==1{print $1}')}
back_in_fastboot() {   # waits for the tablet to leave fastboot, then up to $1 s for it to return
  while present; do sleep 1; done
  local end=$((SECONDS + $1))
  until present; do
    if (( SECONDS > end )); then echo "NOT BACK in fastboot within $1 s: enter it by hand (Power + Volume down)"; end=$((SECONDS + 100000)); fi
    sleep 2
  done
}
fastboot -s "$SERIAL" boot "$IMG" || exit 1
echo "$(date +%T) test image running; up to ${LIMIT}s until it returns to fastboot"
back_in_fastboot "$LIMIT"
echo "$(date +%T) back in fastboot; booting the log reader"
fastboot -s "$SERIAL" flash init_boot_b "$READER_INIT_BOOT" && fastboot -s "$SERIAL" boot "$READER" || exit 1
"$PY" "$ROOT/scripts/read-erhai-log.py" --wait 300 || exit 1
back_in_fastboot 120
echo "$(date +%T) done; the tablet is in fastboot"
