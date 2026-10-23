# erhai touch facts (NT36536 over GENI SPI)

Research note, 2026-10-23. Maps every board fact of sheng's touch driver (`package/TouchBoard.h`, `package/TouchProbe.cpp`,
`drivers/NativeNovatekTouch/*`, `UniXNU/drivers/NativeGeniSpi`, `package/SmmuBypass.h`) to its erhai value and source.
Nothing here has been tried on the device. "Uncertain" marks facts that need a hardware read-back before they are trusted.

Sources used:

- **live DT**: `device-dump/20261023-002031/live.dtb` (running ColorOS kernel), decompiled with `dtc -I dtb -O dts`;
  line numbers below refer to that decompile.
- **gcc-sm8750.c**, **pinctrl-sm8750.c**, **pinctrl-msm.c**, **sm8750.dtsi**: Linux mainline (git.kernel.org torvalds/linux
  master, fetched 2026-10-23).
- **oplus driver**: `oplus/kernel/touchpanel/oplus_touchscreen_v2/Novatek/NT36536_noflash/nvt_drivers_nt36536_noflash.{c,h}`
  and `util_interface/touch_interfaces.c` from LineageOS/android_kernel_oneplus_sm8750-modules (lineage-24.0,
  c691ea9aec0b). This is the driver the stock kernel binds to `novatek,nf_nt36536` (GPL; facts only, no code copied).
- **odm_b**: EROFS image extracted from `device-dump/backup-20261023-013942/parts/super.img` (LP metadata slot 1).

## 1. Summary of what changes

1. **Same bus, same engine, same SMMU stream.** Touch is on QUP1 SE4 (`spi@a90000`), wrapper `0xac0000`, SMMU SID `0xa3`:
   identical addresses to sheng. Different: GCC offsets, TLMM pins (48-51, IRQ 162), TLMM INTR_CFG target bit, IRQ edge
   (falling), SPI clock (12 MHz), apps SMMU size (0x100000).
2. **Different controller protocol.** NT36536 is not NT36532E: other trim id, other event buffer, other CRC/auto-copy
   register bank, multi-header firmware. `NvtProtocol.c` as written would reject the chip (`nvt_identify` matches only
   `xx ?? ?? 32 65 03`) and its firmware parser rejects the erhai image (see 4.3).
3. **No host touch processing.** The erhai firmware reports finger coordinates itself (oplus framework, 10 slots, event
   buffer points). sheng's THP path (5 KB matrix frames, `NvtTouchCore` baseline/peak/centroid) does not apply. The loop
   becomes: interrupt, read 121 bytes at the event buffer, verify checksum, decode up to 10 points.
4. **No reset GPIO and no regulators.** The NT36536 is in-cell (TDDI with the XN242 panel); it is powered and reset by the
   display. Download is only possible while the panel is on (the stock driver sets `lcd_trigger_load_tp_fw_support`).
5. **Firmware found** (section 5): one production image, identical in the DT, the `.img` and the `_FAE.img`.

## 2. Board constants (TouchBoard.h)

| sheng constant | sheng value | erhai value | erhai source | Notes |
| --- | --- | --- | --- | --- |
| `TB_QUP_WRAPPER_BASE` | `0xac0000` | `0x00ac0000` (size 0x2000) | live DT l.24428 `qupv3_1: qcom,qupv3_1_geni_se@ac0000`, `reg = <0xac0000 0x2000>`; sm8750.dtsi l.1558 | same |
| `TB_SE_BASE` | `0xa90000` | `0x00a90000` | live DT l.24707 `qupv3_se4_spi: spi@a90000`, `reg = <0xa90000 0x4000>`; sm8750.dtsi l.1872 `spi4` | same |
| `TB_SE_SIZE` | `0x4000` | `0x4000` | same node | same |
| `TB_QUP_SMMU_SID` | `0xa3` | `0xa3` | live DT `qupv3_1` `iommus = <0x7e 0xa3 0x00>` (0x7e = apps_smmu); sm8750.dtsi `iommus = <&apps_smmu 0xa3 0x0>` | same. GSI DMA (`gpi_dma1`) is SID `0xb6`, not used by SE DMA |
| `TB_APPS_SMMU_BASE` | `0x15000000` | `0x15000000` | live DT l.24254 `apps-smmu@15000000` | same |
| `TB_APPS_SMMU_SIZE` | `0xd0000` | **`0x100000`** | live DT `reg = <0x15000000 0x100000>`; sm8750.dtsi l.5575; `drivers/NativeUfs/SmmuTakeover.h` already uses 0x100000 | `SmmuBypass.h` also hard-codes `0xd0000` in its geometry check, and `TouchBoardTests.c` sizes its fake at 0xd0000: raise both |
| `TB_GCC_BASE` | `0x100000` | `0x00100000` | live DT l.9773 `gcc: clock-controller@100000` (`qcom,sun-gcc`) | same |
| `TB_GCC_VOTE_ENABLE` | `0x52008` | `0x52008` | gcc-sm8750.c: every QUP1 branch below has `enable_reg = 0x52008` | same register |
| `TB_GCC_GPLL0_ENABLE` | `0x52018` | **`0x52020`** bit 0 | gcc-sm8750.c l.52-56 `gcc_gpll0 .enable_reg = 0x52020, .enable_mask = BIT(0)` | still in the 0x52000 page TouchProbe maps |
| `TB_GCC_GPLL0_MODE` | `0x0` | `0x0` (lock = bit 31) | gcc-sm8750.c `gcc_gpll0 .offset = 0x0`, Taycan ELU PLL (PLL_MODE at offset 0) | same |
| `TB_VOTE_WRAP1_CORE_2X` | bit 18 | bit 18 | gcc-sm8750.c l.2049 `gcc_qupv3_wrap1_core_2x_clk` | same bit |
| `TB_VOTE_WRAP1_CORE` | bit 19 | bit 19 | l.2062 `gcc_qupv3_wrap1_core_clk` | same bit |
| `TB_VOTE_WRAP1_M_AHB` | bit 20 | bit 20 | l.2447 `gcc_qupv3_wrap_1_m_ahb_clk` (hwcg) | same bit |
| `TB_VOTE_WRAP1_S_AHB` | bit 21 | bit 21 | l.2462 `gcc_qupv3_wrap_1_s_ahb_clk` (hwcg) | same bit |
| `TB_VOTE_WRAP1_S4` | bit 26 | bit 26 | l.2165 `gcc_qupv3_wrap1_s4_clk` | same bit |
| `TB_HALT_CORE_2X` | `0x23294` | **`0x2315c`** | gcc-sm8750.c `halt_reg = 0x2315c` | |
| `TB_HALT_CORE` | `0x23284` | **`0x23148`** | `halt_reg = 0x23148` | |
| `TB_HALT_M_AHB` | `0x2327c` | **`0x23140`** | `halt_reg = 0x23140` (= hwcg_reg) | |
| `TB_HALT_S_AHB` | `0x23280` | **`0x23144`** | `halt_reg = 0x23144` (= hwcg_reg) | |
| `TB_HALT_S4` | `0x184e4` | **`0x183cc`** | `gcc_qupv3_wrap1_s4_clk .halt_reg = 0x183cc` | page 0x18000, as before |
| `TB_CBCR_CLK_OFF` | bit 31 | bit 31 | clk-branch generic | same |
| `TB_S4_RCG` | `0x184f0` | **`0x183dc`** | l.775 `gcc_qupv3_wrap1_s4_clk_src .cmd_rcgr = 0x183dc`, `mnd_width 16`, `hid_width 5`, `clk_rcg2_shared_ops`; registered as DFS (l.3169 `DEFINE_RCG_DFS(gcc_qupv3_wrap1_s4_clk_src)`) | DFS register layout (0x14 DFSR, 0x1c/0x5c/0x9c perf) is generic clk-rcg2: unchanged |
| `TB_DFS_LEVELS` | 8 | 8 | clk-rcg2 generic | |
| `tb_src_hz` | 0 TCXO 19.2 MHz, 1 GPLL0 600 MHz, 6 GPLL0 even 300 MHz | same | gcc-sm8750.c l.159 `gcc_parent_map_0 = {TCXO 0, GPLL0_OUT_MAIN 1, GPLL0_OUT_EVEN 6}`; l.680 table: 100 MHz = MAIN/6, 75 MHz = EVEN/4, 120 MHz = MAIN/5 | same inference. The table tops out at 120 MHz |
| `TB_TLMM_BASE` | `0x0f100000` | `0x0f100000` | sm8750.dtsi l.3908 `tlmm: pinctrl@f100000`, `reg = <0x0f100000 0x102000>` | **Note** the downstream live DT says `pinctrl@f000000 reg = <0xf000000 0x202000>` (l.1218, `qcom,sun-tlmm`): the downstream driver adds a 0x100000 tile offset itself. Physical pin N control is `0xf100000 + 0x1000*N` either way; uncertain until the read-only stage-1 snapshot shows sane values |
| `TB_GPIO_STRIDE` | `0x1000` | `0x1000` | pinctrl-sm8750.c `REG_SIZE 0x1000`, ctl `+0`, io `+4`, intr_cfg `+8`, intr_status `+0xc` | same |
| `TB_GPIO_SPI_MISO` | 44 | **48** | live DT l.3134 `qupv3_se4_spi_miso_active`: `gpio48`, `qup1_se4_l0` | sm8750.dtsi `qup_spi4_data_clk`: gpio48/49/50 = MISO, MOSI, CLK |
| `TB_GPIO_SPI_MOSI` | 45 | **49** | `qupv3_se4_spi_mosi_active`: `gpio49`, `qup1_se4_l1` | |
| `TB_GPIO_SPI_CLK` | 46 | **50** | `qupv3_se4_spi_clk_active`: `gpio50`, `qup1_se4_l2` | |
| `TB_GPIO_SPI_CS` | 47 | **51** | `qupv3_se4_spi_cs_active`: `gpio51`, `qup1_se4_l3`; touch node also has `cs-gpio = <&tlmm 51 0>` (used by the oplus driver only to drive CS low in sleep) | |
| `TB_GPIO_IRQ` | 25 | **162** | touch node `irq-gpio = <&tlmm 0xa2 0x2002>`, `interrupts = <0xa2 0x2002>`; pinctrl `ts_int_active` (l.6672): gpio162, function `gpio`, drive 8 mA, bias-disable, input-enable | TouchProbe never writes this pin's CTL; on erhai write `func 0, drv code 3 (8 mA), pull 0, OE 0` to be independent of what UEFI left |
| `TB_FUNC_QUP1_SE4` | 1 | 1 | pinctrl-sm8750.c l.1483-1486 `PINGROUP(48..51, qup1_se4, ...)`: function slot 1 | `tb_pin_cfg` packing (mux bit 2, drv bit 6, pull bit 0) unchanged |
| `TB_DRIVE_6MA` | code 2 (6 mA) | 6 mA on all four is what mainline uses (sm8750.dtsi l.4198-4211, bias-disable) | downstream live DT: MISO 6, MOSI 4, CLK 6, CS 4 mA, each with both `bias-pull-down` and `bias-disable` | Recommend mainline (6 mA, pull disabled) = the sheng value. Which bias wins downstream is ambiguous |
| `TB_SPI_MODE` | 0 | 0 | oplus driver l.9299 `ts->s_client->mode = SPI_MODE_0`, `bits_per_word = 8`; no `spi-cpha`/`spi-cpol` in the DT | same; the mode sweep in TouchProbe is still a useful cross-check |
| `TB_SPI_DEFAULT_HZ` / `TB_SPI_MAX_HZ` | 19.2 MHz | **12 MHz** (`0xb71b00`) | touch node `spi-max-frequency = <0xb71b00>`; controller `spi-max-frequency = <0x2faf080>` (50 MHz) | reachable exactly from 48/96/120 MHz DFS levels if present, else 100/9 = 11.1 MHz |

## 3. Other hard-coded facts (TouchProbe.cpp, GeniSpi, SmmuBypass)

| Where | sheng | erhai | Source / note |
| --- | --- | --- | --- |
| TLMM summary interrupt (`armcpu_spi_attach(32 + 208)`) | SPI 208, INTID 240 | SPI 208 (`0xd0`), INTID 240 | live DT tlmm `interrupts = <0x00 0xd0 0x04>`; sm8750.dtsi l.3912 `GIC_SPI 208` |
| INTR_CFG target field (`armTouchIrq`: `3u << 5`) | bits 7:5 | **bits 10:8** (`intr_target_bit = 8`, KPSS value 3) | pinctrl-sm8750.c PINGROUP macro; sm8550 had `intr_target_bit = 5`. Bits 6/7 are now `intr_wakeup_present`/`intr_wakeup_enable`: writing `3u << 5` on erhai would set the wakeup bits and leave the target at 0. Mask must cover bits 5..10 |
| Edge (`armTouchIrq`: rising, `1u << 2`, polarity) | rising (`0x2001`) | **falling** (`0x2002` = `IRQF_TRIGGER_FALLING|IRQF_ONESHOT`) | touch node flags. pinctrl-msm.c `msm_gpio_irq_set_type`: falling = detection `2 << 2` plus polarity bit 1; raw status bit 4 on. So `INTR_CFG = (3 << 8) | (2 << 2) | (1 << 1) | (1 << 4)`, then `| 1` to enable |
| Wakeup/PDC routing | n/a | gpio162 has a PDC wake map (`{162, 109}`, pinctrl-sm8750.c l.1674); live tlmm has `wakeup-parent = pdc` | Uncertain: mainline sets `intr_wakeup_enable` (bit 7) only for PDC-routed wake IRQs. For the summary path leave bit 7 clear; if the summary IRQ never fires, try with it set, and keep the polling fallback (`sheng-touch-poll`) |
| SE interrupt | (unused, polled) | SPI 357 (`0x165`) | live DT `interrupts = <0x00 0x165 0x04>`; sm8750.dtsi `GIC_SPI 357` |
| SmmuBypass bank choice | last context bank, shared by every bypass stream | same mechanism | erhai's UFS planner (`drivers/NativeUfs/SmmuTakeover.h`) falls back to lower banks when the top bank is held. The touch planner must target the bank UFS actually configured (or share UFS's code), otherwise it reports "reserved context bank is not a bypass bank" or "conflicting" |
| SmmuBypass geometry limit | `(pages + banks) * 4096 <= 0xd0000` | `<= 0x100000` | see `TB_APPS_SMMU_SIZE` |
| DMA buffer mask (`0xfffffff000`) | 40-bit | keep | bounded at run time by SMMU IDR2; no erhai difference known |
| GENI FIFO vs DMA | v53-v55: FIFO_IF_DISABLE = 1, SE DMA used | Uncertain, read at stage 2 (`GENI_IF_DISABLE_RO`) | the downstream node lists GSI `dmas` (gpi_dma1), as sheng's did. Both paths exist in GeniSpi |
| Interconnect votes | none | none (DT: `qup-core`, `qup-config`, `qup-memory`) | XNU votes nothing on either board; sheng worked without. Risk: if a bus is collapsed after UEFI, stage 2 will show it (engine read hangs) |
| CPU-side constants (`19200000` fallback cntfrq) | 19.2 MHz | 19.2 MHz | project CLAUDE.md timebase |
| Publish axes (`20319 - y`, 30480/20320) | sheng THP axes, 0.1 px of 3048x2032 | firmware reports **24000 x 33920** (0.1 px of 2400x3392 portrait) | touch node `touchpanel,panel-coords = <0x5dc0 0x8480>`, `touchpanel,display-coords = <0x960 0xd40>`. `UniXNU/drivers/NativeHidInput/NativeHidInput.cpp` l.172 hard-codes 30480/20320 and `package/BootProgress.c` l.1137-1159 too: either rescale in TouchProbe (long 33920 -> 30480, short 24000 -> 20320) or parameterise those. Orientation: the panel scans out 3392x2400 landscape (two 1696x2400 split-link halves, live DT l.40092 XN242 node) while touch x runs along the 2400 axis: published long axis = touch y, short axis = touch x, with the mirror direction unknown until a photographed test (as sheng's v94) |

## 4. NT36536 vs NT36532E protocol

All from the oplus driver unless noted. The bus framing is identical to sheng's: set page `{0xFF, addr>>15, addr>>7}`;
write `{addr & 0x7F | 0x80, data...}`; read `{addr & 0x7F, dummy...}` with data from rx byte 2 (`touch_interfaces.c`
l.892 `CTP_SPI_READ`: `memcpy(buf + 1, rbuf + 2, len - 1)`).

### 4.1 Identification

| Item | NvtProtocol.c (NT36532E) | NT36536 (oplus l.40, l.284, l.731) |
| --- | --- | --- |
| `chip_ver_trim` / `swrst_sif` | `0x1FB104` / `0x1FB43E` | same; plus `bld_spe_pups_addr = 0x1FB535` (CRC-reboot recovery) |
| Engine reset before identify | `0x7FFF80 <- 0x5A` | not done (`ENG_RST_ADDR = 0x7FFF80` is set at probe but never written). Harmless to keep, not required |
| Bootloader reset | `swrst_sif <- 0x69`, 5 ms | same (`nvt_bootloader_reset_noflash`, l.369) |
| ID read | write 7 bytes at trim addr, read 6 | same |
| ID match | `id[3..5] = 32 65 03` (NT36532/E) | **`{0x14, x, x, 0x23, 0x65, 0x03}` mask `1,0,0,1,1,1`** or **`{0x15, 0x0E, x, 0x23, 0x65, 0x03}` mask `1,1,0,1,1,1`** |
| Single/cascade | `ENB_CASC 0x1FB12C` bit 0 | same register; both NT36536 maps are identical, so the choice does not matter |
| HW info | 3-byte HW CRC, auto copy via `TX_AUTO_COPY_EN`, no multi-header | 3-byte HW CRC, `CHECK_TX_AUTO_COPY_EN`, **`BLDMHEADER_SUPPORT`** (only affects CRC diagnostics, l.1504) |

### 4.2 Memory map (`NT36536_single_memory_map`, oplus l.103)

| Field | NvtProtocol.c NT36532E-cascade | NT36536 |
| --- | --- | --- |
| `event_buf` | `0x11C400` | **`0x130900`** |
| `boot_rdy` | `0x1FB50D` | `0x1FB50D` |
| `tx_auto_copy_en` | `0x1FC925` | **`0x1FC825`** |
| `spi_dma_tx_info` | `0x1FC914` | **`0x1FC814`** |
| `ilm_des` / `ilm_length` | `0x1FB528` / `0x1FB518` | **`0x1FCC30` / `0x1FCC34`** |
| `dlm_des` / `dlm_length` | `0x1FB52C` / `0x1FB530` | **`0x1FCC40` / `0x1FCC44`** |
| `g_ilm_checksum` / `g_dlm_checksum` | `0x1FB500` / `0x1FB504` | **`0x1FCC38` / `0x1FCC48`** |
| `r_ilm_checksum` / `r_dlm_checksum` | `0x1FB520` / `0x1FB524` | **`0x1FCC7C` / `0x1FCC80`** |
| `bld_ilm_dlm_crc` | `0x1FB533` | **`0x1FB532`** (`DMA_CRC_FLAG 0x1FB534`, `DMA_CRC_EN 0x1FB51B`) |
| `enb_casc` | `0x1FB12C` / `0x01` | same |
| `NVT_DOZE_DELAY_ADDR` | `0x113718` (sheng) | no equivalent; do not write |

### 4.3 Firmware format and download

- Header parser (`nvt_bin_header_parser`, l.1012) differs from `nvt_fw_parse`: it first shortens the header end to the
  first info entry with `size != 0 && bin == 0` (`tmp_end = pos + 0x10`), counts info sections up to there, and for a
  cascade-2nd-header image doubles `tmp_end`. ILM/DLM CRCs are taken from the file (`0x18`, `0x1C`).
  On the erhai image this yields `tmp_end = 0x200`, 14 info sections, 16 partitions, of which 4 are written:

  | Partition | SRAM | size | bin offset | crc |
  | --- | --- | --- | --- | --- |
  | ILM | `0x000000` | `0x1FE43` | `0x250` | `a6b863ae` |
  | DLM | `0x100000` | `0x159F7` | `0x22000` | `df14ad7e` |
  | Header | `0x1159F8` | `0xFF` | `0x000` | (`55855333`) |
  | Header (2nd) | `0x115AF8` | `0xFF` | `0x100` | (`cb35a8ee`) |

  sheng's parser (header end = word 0 = `0x250`) walks into the extra block at `0x200-0x250` and hits an entry with
  `bin = 0xFFFFFFFF`, so it returns `NVT_ERR_FW`. The file's ILM/DLM CRCs equal `nvt_crc32_words` (checked), so
  only the section walk needs the NT36536 rule.
- File integrity used by the vendor: end flag `"NVT"` at the end of the last used 4 KiB sector (`fw_need_write_size =
  0x3E000`), `FW_VER + ~FW_VER = 0xFF` at `need_write - 4K` (here `0x1D`/`0xE2`), and a byte-sum checksum over
  `size - 4` bytes stored in the last 4 bytes (`0xFEF8A0EA`, checked).
- Download sequence (`Download_Firmware_HW_CRC`, l.1727) = sheng's: bootloader reset; write ILM/DLM DES/LEN/golden
  CRC (3/3/4 bytes, NT36536 addresses); cascade-2nd-header -> `TX_AUTO_COPY_EN <- 0x56`; write partitions (`size + 1`
  bytes each; vendor chunks at 1 KiB with a page set per chunk, sheng uses 63 KiB transfers: keep 1 KiB as the fallback
  if SRAM read-back shows corruption); poll `TX_AUTO_COPY_EN` until 0 (200 x 1 ms); clear `event_buf+0x60`, write
  `0xAE` to `event_buf+0x50`; `BOOT_RDY <- 1`, 5 ms, page = event buffer; poll `event_buf+0x60` for `0xA0..0xAF`
  (10 x 10 ms). On failure the vendor reads one byte and, if `0xFC`, runs the CRC-reboot stop (`swrst_sif <- 0xAA` twice,
  `0x1FB535 <- 0xA5`).
- After download: firmware info at `event_buf + 0x78` (39-byte read): `fw_ver` with complement, `x_num`, `y_num`,
  `abs_x_max`, `abs_y_max`, **event-buffer protocol byte** (oplus `buf[13]`, i.e. byte 12 of the info block; sheng's
  `nvt_get_fw_info` stores byte 13 as `fw_type`, which is oplus' `fw_sub_ver`), PID at bytes 34/35.
  `0xF1` selects the high-resolution point format.
- No post-boot commands are needed for normal touch: `nvt_mode_switch(MODE_NORMAL)` is a no-op. sheng's `0x19`
  (idle baseline), doze delay and `0x01/0x02` doze commands are THP-firmware specific; do not send them.

### 4.4 Touch report (replaces THP frames and NvtTouchCore)

- On each falling edge of gpio162: read `POINT_DATA_LEN + 1 = 121` bytes at offset 0 of the event-buffer page
  (`0x130900`) (`nvt_trigger_reason`, l.1927).
- Patterns: bytes 1..6 all `0x77` = firmware ESD/recovery request; bytes 1..6 all `0xFD`/`0xFE` repeatedly = watchdog
  reset (vendor reloads firmware).
- Finger checksum: `sum(buf[1..64])` two's complement must equal `buf[65]`. Pen data `buf[66..79]` with its own
  checksum (pen not needed).
- Points: 10 slots (`touchpanel,max-num-support = <0x0a>`), 6 bytes each at `buf[1 + 6*i]`:
  `id = (b0 >> 3) - 1`, `status = b0 & 7` (1 enter, 2 move = down; 3 FOD, 5 palm, 6 hold), then
  - high-res protocol (`0xF1`, l.2168): `x = b1 << 8 | b2`, `y = b3 << 8 | b4`, `w = b5`;
  - legacy: `x = b1 << 4 | b3 >> 4`, `y = b2 << 4 | b3 & 0xF`, `w = b4`, pressure `b5` (+ `buf[63+i] << 8` for i < 2).
  The legacy 12-bit format cannot reach 24000/33920, so the high-res format is expected (uncertain until fw info is
  read on the device).
- Sensor grid for reference: `touchpanel,tx-rx-num = <60 40>` (same node count as sheng's 40x60 matrix, irrelevant
  without THP).

## 5. Firmware

Location on the device: `/odm/firmware/tp/24924/` (odm_b, EROFS, lz4 compact-compressed). The project directory is
`24924` (`platform_support_project_dir = <0x615c ...>`), although `oplusboot.prjname=24926`. The DT `firmware_name` is
`0x2XN242CN00`; the oplus framework requests `tp/24924/FW_NT36536_HUAXING.img` and falls back to the DT copy
(`fw_update_in_probe_with_headfile`, property `firmware-data-0`).

Copied (read-only extraction, gitignored) to `UniXNU-erhai-ng/.cache/firmware/touch/`:

| File | Size | SHA-256 | What it is |
| --- | --- | --- | --- |
| `FW_NT36536_HUAXING.img` | 258256 | `01d296ef9cb099879f0226b7533d3e169a3e2c726c2402a0f3f53666366d636c` | as on odm: 0x50-byte oplus `FwUp` header (magic `FwUp`, size `0x3F000`, name `tp/24924/FW_NT36536_HUAXING.img`) + 0x3F000 image + 0x80-byte trailer (signature) |
| `FW_NT36536_HUAXING.body.bin` | 258048 | `3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b` | the `.img` with header and trailer stripped (`[0x50, 0x3F050)`) |
| `FW_NT36536_HUAXING_FAE.img` | 258048 | `3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b` | raw image, byte-identical to the body |
| `nt36536_dt_firmware-data-0.bin` | 258048 | `3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b` | live DT `firmware-data-0`, byte-identical |
| `FW_NT36536_HUAXING_TEST.bin` | 258048 | `6d575148f20025da426348caf984d5d788a050fe35b6f72efc2527251511b778` | factory test firmware (different ILM/DLM sizes); do not use |
| `LIMIT_NT36536_HUAXING.img` | 200280 | `9be6f18bc3da7f97ca1807836f2ac2ebed22bee78731f0ef25b7c0d023581089` | self-test limits, not firmware |

Use `FW_NT36536_HUAXING_FAE.img` (or the body) as the download image: FW version `0x1D`, end flag `NVT`, file checksum
and ILM/DLM CRCs verified. `scripts/gen-touch-firmware.py` defaults to the sheng path
`.cache/firmware/novatek/novatek_nt36532_n81a_fw_csot.bin`; erhai needs it pointed at this file (and
`proprietary-files.txt` still names the sheng blob).

Extraction recipe (no new software): parse LP metadata slot 1 of `super.img` for `odm_b` extents, concatenate them,
walk the EROFS directory tree, and decode the `COMPRESSED_COMPACT` (lz4, big-pcluster, 0-padding) file with a small
Python reader. The reader was validated by the `_FAE.img` result matching the DT blob byte for byte.

## 6. Bring-up sequence differences

1. Stage 1 (read-only) is unchanged except the new offsets; it should additionally read gpio162 CTL/INTR_CFG and the
   PLL0 enable vote at `0x52020`.
2. Stage 2: vote GPLL0 (`0x52020` bit 0) and the five QUP branches (`0x52008` bits 18-21, 26); poll the new halt
   registers; mux gpio48-51 to function 1; configure gpio162 as input. No regulator, no reset GPIO.
3. Stage 3: SPI mode 0 at <= 12 MHz; identify with the NT36536 trim ids; no reset pin to toggle (in-cell: the display
   owns reset; the panel must be on).
4. Stage 4: NT36536 parser, NT36536 register bank, then the coordinate loop (4.4) instead of THP frames. Re-download is
   needed whenever the display is power-cycled (`lcd_trigger_load_tp_fw_support`); the ESD patterns in 4.4 signal it.

## 7. Open items (need device evidence)

- Whether `0xf100000` is the right physical TLMM base on this firmware (downstream DT shows `0xf000000` with a 0x202000
  window). Stage 1 values decide.
- Whether summary SPI 208 fires for gpio162 with `intr_wakeup_enable` clear.
- Which apps SMMU context bank the UFS driver ended on, and the real IDR1 geometry.
- FIFO vs DMA mode of SE4 and which DFS levels the firmware programmed for `0x183dc`.
- Event-buffer protocol byte (`0xF1` expected) and the touch-to-display orientation/mirroring.
- Whether 63 KiB SPI writes are accepted by the NT36536 bootloader (vendor uses 1 KiB).
