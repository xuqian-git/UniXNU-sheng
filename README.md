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
| Display | ⚠️ | CPU-rendered (QuartzCore's CPU renderer, no GPU) into NativeLegacyDisplay on the GOP framebuffer (3392x2400); cached source reads cut the per-frame copy from ~170 ms to ~16 ms (`patches/NativeMobileDisplay-fast-copy.patch`). Settings stays white (CPU renderer, as on UniXNU's QEMU without Metal) |
| SpringBoard / lock screen | ✅ | backboardd and SpringBoard stay up; the lock screen is shown (2026-10-23) |
| Home screen | ✅ | reachable by swiping up; apps launch (2026-10-23) |
| Touch (Novatek NT36536) | ✅ | `ErhaiTouchProbe`: QUP1 SE4 SPI with SE DMA, firmware download each boot, HID multitouch; taps and swipes verified (`erhai-touch-orient=6`) |
| CPU DVFS and thermal | ✅ | `ErhaiCpuFreq`: SCMI performance protocol through the CPUCP mailbox; load governor and TSENS limit with stock ColorOS targets (80% load, hispeed, floors, 2918/3072 MHz sustained cap, 75 C) |
| GPU (Adreno 830) | ❌ | |
| Wi-Fi, Bluetooth, audio, camera, USB, battery, suspend | ❌ | |

✅ working, ⚠️ partial, 🚧 in progress, ❌ not started. Last update: 2026-10-23.

## Layout of this branch

| File | Purpose |
| --- | --- |
| `board.toml` | erhai board: IPSW, userdata LBAs, archives, prelink, firmware, payload, kernel arguments, text fixups |
| `configs/erhai_defconfig` | Kernel configuration (sheng's, without Adreno) |
| `drivers/NativeUfs` | SM8750 override of the base NativeUfs driver |
| `scripts/make-erhai-adt.py` | Apple device tree for erhai |
| `scripts/package-erhai-boot.py` | Boot image with the payload embedded after the firmware volume |

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

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE).
