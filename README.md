<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="assets/logo-light.svg">
    <img alt="UniXNU" src="assets/logo-light.svg" width="200">
  </picture>
</p>

<p align="center">
  <a href="https://unixnu.com"><img alt="Documentation" src="https://img.shields.io/badge/docs-unixnu.com-2A5BD7?logo=readthedocs&logoColor=white"></a>
  <a href="https://t.me/unixnu"><img alt="Telegram" src="https://img.shields.io/badge/Telegram-@unixnu-26A5E4?logo=telegram&logoColor=white"></a>
  <a href="https://qun.qq.com/universal-share/share?ac=1&authKey=Pubz23DOCY76nxjlGuSms2zesk4jRvas4zrXJRYk7omf7CprXbGQigLEI%2BCoYR8D&busi_data=eyJncm91cENvZGUiOiIxMTI2MTE2NzU5IiwidG9rZW4iOiJwSHJsUDN1UnN2Vi8yM210TjJRbzBIcERmcUpyOS9lb0hHVVpadWdvVFdScVpSYUFLT1YzTk5TWC9mN2xWYmtIIiwidWluIjoiMzE1MTEwOTc0MSJ9&data=_cJgr-jX9yJ2-GXwVjRN5FOhDyk5B-HAfIk5LSunbW_XkkmKqNXbpH33wzP93J0p9DI1fbjgBUrUfTVhwDcxjg&svctype=4&tempid=h5_group_info"><img alt="QQ Group" src="https://img.shields.io/badge/QQ%20Group-join-12B7F5?logo=qq&logoColor=white"></a>
  <a href="https://x.com/unixnu"><img alt="X" src="https://img.shields.io/badge/X-@unixnu-000000?logo=x&logoColor=white"></a>
</p>

# UniXNU-erhai (OnePlus Pad 2 Pro)

UniXNU port to the **OnePlus Pad 2 Pro** (erhai, OPD2413, SM8750), booting the unchanged iPadOS 17.7.11 (21H461) image
of the iPad7,1. This is the `erhai` branch of a fork of [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng): the sheng
package is shared unchanged and the branch only adds the board. It tracks `UniXNU-sheng` main and the latest
[UniXNU](https://github.com/UniXNU/UniXNU) main. Community project, not maintained by the UniXNU organization.

一加平板 2 Pro（erhai，SM8750）的 UniXNU 移植，运行未修改的 iPad7,1 iPadOS 17.7.11 镜像。本分支基于 UniXNU-sheng 的 fork，
跟随 sheng 与 UniXNU 最新上游，只增加 erhai 板级部分。

## Progress / 进度

| Stage | State | Notes |
| --- | --- | --- |
| UEFI (erhaiPkg) and payload handoff | ✅ | Own Mu-Silicium platform; payload embedded in the boot image kernel field (the OnePlus ABL replaces the ramdisk) |
| XNU entry, GIC, timer | ✅ | |
| SMP (8 Oryon cores) | ✅ | Prime cores need MPIDR Aff2 in XNU's CPU id (UniXNU patch pending upstream) |
| Native UFS | ✅ | SM8750 controller: identity stage-1 SMMU context, Hibern8 exit before the first command |
| UFS root (verified OS + SystemOS windows on userdata) | ✅ | |
| launchd and userspace | ✅ | Needed native PAN (Oryon implements PAN) |
| Data volume | ✅ | Persistent: the store window at the end of userdata (`sheng-data-persist=2`), seed grown to 210 GiB with `seed-data-grow.py --auto`; resumes across boots with no I/O errors |
| Display | ⚠️ | The render server composites through the Adreno 830 (S6d) into NativeLegacyDisplay on the GOP framebuffer (3392x2400), at 120 Hz (S6g), held at 8 bits per channel. Short stalls remain; iconservicesagent crashes |
| SpringBoard / lock screen | ✅ | backboardd and SpringBoard stay up; the lock screen is shown (2026-10-23) |
| Home screen | ✅ | reachable by swiping up; apps launch (2026-10-23) |
| Touch (Novatek NT36536) | ✅ | `ErhaiTouchProbe`: QUP1 SE4 SPI with SE DMA, firmware download each boot, HID multitouch; taps and swipes verified (`erhai-touch-orient=6`) |
| CPU DVFS and thermal | ✅ | `ErhaiCpuFreq`: SCMI performance protocol through the CPUCP mailbox; load governor and TSENS limit with stock ColorOS targets (80% load, hispeed, floors, 2918/3072 MHz sustained cap, 75 C) |
| GPU (Adreno 830) | ⚠️ | Native bring-up: GMU, GX power, zap shader, CP, IRQ fence wait, DCVS 160-900 MHz, 4 GiB GPU VA (S1-S6f). Metal through an owner plugin running Mesa 26.1 Turnip; fill, compute and triangle pass (S6c) |
| Wi-Fi, Bluetooth, audio, camera, USB, battery, suspend | ❌ | |
| Unattended test runs | ✅ | XNU restarts into fastboot by itself (`sheng-autoreset`, PMIC reboot-reason cell); a Linux log reader on init_boot_b hands the UFS log window, the diagnostic export and a screenshot to the host as a USB volume (2026-10-24) |

✅ working, ⚠️ partial, 🚧 in progress, ❌ not started. Last update: 2026-10-24.

## Layout of this branch

| File | Purpose |
| --- | --- |
| `board.toml` | erhai board: IPSW, userdata LBAs, archives, prelink, firmware, payload, kernel arguments, text fixups |
| `configs/erhai_defconfig` | Kernel configuration (sheng's, without Adreno) |
| `drivers/NativeUfs` | SM8750 override of the base NativeUfs driver |
| `scripts/make-erhai-adt.py` | Apple device tree for erhai |
| `scripts/package-erhai-boot.py` | Boot image with the payload embedded after the firmware volume |
| `package/linux-log/init-erhai`, `rebootbl-erhai` | Log reader: the stock GKI kernel and vendor modules, USB mass storage (logs) plus an ACM shell (debugging) |
| `package/linux-log/splashcon` | Kernel console on the boot logo's framebuffer, for bring-up of the reader's early steps |
| `scripts/build-erhai-log-reader.py`, `build-erhai-splashcon.sh` | Build the reader image and its init_boot ramdisk, and splashcon for the stock kernel |
| `scripts/run-erhai-unattended.sh`, `read-erhai-log.py` | One test run end to end; fetch and decode the log off the reader's volume |

## Build

From the UniXNU checkout, with this repository at `../UniXNU-erhai` and the erhai UEFI at `../UEFI-erhai`
(not published; it contains binaries extracted from the stock firmware):

```sh
./unixnu defconfig erhai_defconfig -D ../UniXNU-erhai
./unixnu fetch
./unixnu ipsw fetch
ln -sfn "$PWD/../UEFI-erhai" out/erhai/tree/uefi-sheng   # sheng's BootProgress takes Font.h from the UEFI checkout
./unixnu build kernel
# from out/erhai/tree, with build/userdata-root/userdata-root.json of the flashed root image:
python3 scripts/seed-data-grow.py --auto --userdata-bytes 237866397696 <seed>.dbr <seed-full>.dbr
./unixnu build payload --unique-chip-id <hex> --trust-cache <bundle> --data-ramdisk <seed-full>.dbr
python3 ../UniXNU-erhai/scripts/package-erhai-boot.py --uefi ../UEFI-erhai --dtb <erhai.dtb> \
    --payload out/erhai/payload/payload.img -o out/erhai/boot/boot.img
fastboot boot out/erhai/boot/boot.img
```

Boot with `fastboot boot` only; do not flash boot partitions. The userdata partition holds the root image.

## Unattended test runs / 无人值守测试

`board.toml` turns on the UFS diagnostic log (`sheng-log=1`). A test image built with
`--kernel-arg sheng-autoreset=120 --kernel-arg sheng-diag-export=110` restarts into fastboot by itself after 120 s, or after a
panic through the hypervisor watchdog. The log reader runs the tablet's own stock kernel and vendor modules. ABL takes the generic
ramdisk from init_boot_b, so the reader's ramdisk is flashed there (`init_boot_b` is the only partition it changes; keep a backup).
The reader copies the log window and the export region below it into a FAT volume, `ERHAILOG`, which it offers read-only over USB.

```sh
# the reader, once per stock firmware (busybox: a static aarch64 build, e.g. Debian's busybox-static)
python3 scripts/build-erhai-log-reader.py --boot boot_b.img --vendor-boot vendor_boot_b.img --busybox busybox \
    --out ~/.cache/erhai-log-reader/erhai-log-reader.img --init-boot ~/.cache/erhai-log-reader/erhai-log-reader-init_boot.img
# one run: boot, wait for fastboot, read the log into build/erhai-logs/<tag>-<boot id>/, back in fastboot
PY=python3.12 scripts/run-erhai-unattended.sh out/erhai/boot/boot.img
```

`read-erhai-log.py` writes `msgbuf.txt`, `slots.txt`, `latest.txt`, `export/` and `diag/`, with `diag/files/screen.bgrx` as a
half-resolution screenshot, plus the reader's own `reader/` files. The eject at the end sends the tablet back to fastboot. While the
reader runs, `/dev/cu.usbmodemerhailog*` is a shell on it, and `touch /run/hold` there keeps it up after the eject. The stock kernel
has no devtmpfs, so the reader makes its device nodes by hand.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE).
