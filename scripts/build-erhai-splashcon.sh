#!/usr/bin/env bash
# Builds package/linux-log/splashcon for the tablet's stock GKI kernel, in an arm64 Debian container (Docker): the Android common kernel
# at the commit the kernel's release names, the kernel's own IKCONFIG, its release string as the vermagic, and the GKI build's
# vmlinux.symvers as the CRCs (MODVERSIONS is on). Inputs and output stay in $GKI (default ~/.cache/erhai-gki), outside Git.
# usage: build-erhai-splashcon.sh BOOT_IMG     (the stock boot image; prints the path of splashcon.ko)
# needs: $GKI/src (android.googlesource.com/kernel/common at that commit), $GKI/vmlinux.symvers (ci.android.com, the build number of
# the release), and a container named erhai-gki-build with clang, lld, make, flex, bison, bc, libelf-dev, libssl-dev, gcc and dwarves
# (pahole: without it olddefconfig drops DEBUG_INFO_BTF_MODULES, which changes struct module, and the kernel refuses the module), $GKI
# on /gki.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
GKI=${GKI:-$HOME/.cache/erhai-gki}
BOOT=${1:?boot image}
python3 - "$BOOT" "$GKI" <<'EOF'
import gzip, re, struct, sys
from pathlib import Path
boot, gki = Path(sys.argv[1]).read_bytes(), Path(sys.argv[2])
(size,) = struct.unpack_from('<I', boot, 8)
kernel = boot[4096:4096 + size]
start = kernel.index(b'IKCFG_ST') + 8
(gki / 'ikconfig').write_bytes(gzip.decompress(kernel[start:kernel.index(b'IKCFG_ED', start)]))
release = re.search(rb'Linux version (\S+)', kernel).group(1)
(gki / 'release').write_bytes(release + b'\n')
EOF
echo "kernel release $(cat "$GKI/release")"
rm -rf "$GKI/mod" && cp -R "$ROOT/package/linux-log/splashcon" "$GKI/mod"
docker exec erhai-gki-build bash -euc '
  K="make -C /gki/src O=/gki/out ARCH=arm64 LLVM=1 -j$(nproc)"
  mkdir -p /gki/out && cp /gki/ikconfig /gki/out/.config
  # The trimmed export list is a kleaf output; it only narrows what vmlinux exports, which vmlinux.symvers already states.
  /gki/src/scripts/config --file /gki/out/.config -d TRIM_UNUSED_KSYMS -d MODULE_SIG_ALL
  $K olddefconfig >/dev/null
  # Only the toolchain versions and the two switches above may differ from the kernel'"'"'s own configuration.
  if diff <(grep "^CONFIG\|^# CONFIG" /gki/ikconfig | sort) <(grep "^CONFIG\|^# CONFIG" /gki/out/.config | sort) |
       grep "^[<>]" | grep -v "_VERSION=\|CONFIG_CC_VERSION_TEXT\|CC_HAS_COUNTED_BY\|TRIM_UNUSED_KSYMS\|UNUSED_KSYMS_WHITELIST\|MODULE_SIG_ALL"; then
    echo "the build configuration differs from the kernel'"'"'s"; exit 1
  fi
  $K modules_prepare >/dev/null
  R=$(cat /gki/release)
  echo "$R" > /gki/out/include/config/kernel.release
  echo "#define UTS_RELEASE \"$R\"" > /gki/out/include/generated/utsrelease.h
  cp /gki/vmlinux.symvers /gki/out/Module.symvers
  $K M=/gki/mod modules >/dev/null
  modinfo /gki/mod/splashcon.ko | grep vermagic'
echo "$GKI/mod/splashcon.ko"
