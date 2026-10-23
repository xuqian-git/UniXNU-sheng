// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of the erhai (OnePlus Pad 2 Pro, SM8750) NT36536 touch controller. `erhai-touch=<n>`:
//   1  read-only snapshot of the GCC branches, GPLL0, the S4 DFS levels and the touch pins
//   2  vote the QUP clocks, wait for them, configure the pins, read the GENI engine identity
//   3  GENI SPI init and clock (SE DMA through the UFS identity SMMU context when the FIFO is disabled), identify the chip
//   4  host-download the firmware, read its info block, arm the interrupt and publish the reported contacts
// The bus side follows sheng's TouchProbe.cpp; the controller reports coordinates itself (no THP frames). Every stage reports
// in native_touch_rows (diagnostic page and banner); the first failure stops the sequence. Board facts and their sources are
// in docs/erhai-touch-facts.md. Nothing here is validated on the device until a boot has shown it.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "NvtProtocol.h"
#include "NvtTouchCore.h"
#include "Nt36536.h"
#include "TouchBoard.h"
extern const uint8_t erhai_touch_firmware[] __attribute__((weak_import));
extern const uint32_t erhai_touch_firmware_size __attribute__((weak_import));
// TouchProbe.cpp: the status rows the diagnostic page and banner show, the stage counters, and the publish path to HID.
extern char native_touch_rows[4][120];
extern volatile uint32_t native_touch_stage_req, native_touch_stage_done, native_touch_step, native_touch_frames,
    native_touch_valid, native_touch_contacts, native_touch_hid_reports;
extern volatile int native_touch_error;
extern volatile int native_touch_last_x, native_touch_last_y;
void native_touch_publish(unsigned count, const struct nvt_contact *contacts);
// drivers/NativeUfs (erhai): put a stream on the UFS identity stage-1 context.
int native_ufs_smmu_share(uint16_t stream, uint32_t *slot, uint32_t *bank) __attribute__((weak_import));
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
}

// erhai board facts (docs/erhai-touch-facts.md; same QUP1 SE4 engine, wrapper and SMMU stream as sheng).
#define ETB_QUP_WRAPPER_BASE 0x00ac0000ULL
#define ETB_SE_BASE 0x00a90000ULL
#define ETB_SE_SIZE 0x4000ULL
#define ETB_QUP_SID 0xa3
#define ETB_GCC_BASE 0x00100000ULL
#define ETB_GCC_VOTE_ENABLE 0x52008            // gcc-sm8750.c: enable_reg of every QUP1 branch below
#define ETB_GCC_GPLL0_ENABLE 0x52020           // gcc_gpll0 enable_reg, bit 0
#define ETB_GCC_GPLL0_MODE 0x0
#define ETB_VOTE_ALL ((1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 26))   // core_2x, core, m_ahb, s_ahb, s4
#define ETB_HALT_CORE_2X 0x2315c
#define ETB_HALT_CORE 0x23148
#define ETB_HALT_M_AHB 0x23140
#define ETB_HALT_S_AHB 0x23144
#define ETB_HALT_S4 0x183cc
#define ETB_S4_RCG 0x183dc                     // gcc_qupv3_wrap1_s4_clk_src, DFS-managed
#define ETB_TLMM_BASE 0x0f100000ULL            // sm8750.dtsi pinctrl@f100000 (the downstream DT's 0xf000000 window adds a tile offset)
#define ETB_GPIO_SPI_MISO 48                   // qupv3_se4_spi: gpio48/49/50/51 = MISO, MOSI, CLK, CS
#define ETB_GPIO_IRQ 162                       // touch irq-gpio = <&tlmm 162 0x2002>: falling edge
#define ETB_FUNC_QUP1_SE4 1
#define ETB_DRIVE_6MA 2
#define ETB_DRIVE_8MA 3
#define ETB_TLMM_SUMMARY_SPI 208               // tlmm interrupts = <GIC_SPI 208>
#define ETB_SPI_MODE 0
#define ETB_SPI_HZ 12000000u                   // touch node spi-max-frequency = <12000000>
#define ETB_FW_X_MAX 24000                     // touchpanel,panel-coords: 0.1 px of the portrait 2400 x 3392 panel
#define ETB_FW_Y_MAX 33920
#define ETB_PUB_X_MAX 30480                    // TouchProbe.cpp / NativeHidInput published axes (long, short)
#define ETB_PUB_Y_MAX 20320

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

constexpr size_t kDmaBytes = 64 * 1024;
void cacheRange(void *p, size_t len, int op) {   // 0 clean, 1 clean+invalidate, 2 invalidate
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t len) {cacheRange(p, len, 0);}
void dmaCleanInval(void *, void *p, size_t len) {cacheRange(p, len, 1);}
void dmaInval(void *, void *p, size_t len) {cacheRange(p, len, 2);}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
// Short status polls spin on the counter instead of blocking the thread (sheng int-182).
void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}

struct Session {
    Region gccLow, gccS4, gccHalt, gccVote, tlmmIrq, tlmmSpi, wrapper, se;
    IOBufferMemoryDescriptor *dmaMem;
    uint64_t dmaPhys;
    int smmuRc;
    uint32_t smmuSlot, smmuBank;
    struct geni_spi spi;
    uint8_t *tx, *rx;
    struct nvt_chip chip;
    struct nvt_bus bus;
    struct nvt_fw fw;
    struct nt36536_info info;
    uint32_t vals[24];
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned level, div;
    uint64_t actualHz;
    int lastRc;
    uint32_t lastLen, xIrq, xGeni;
    // Report loop evidence.
    uint32_t reports, ok, crc, esd, wdt, downloads, busErr;
    uint8_t lastReport[12];
};

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    int rc = geni_spi_transfer(&s->spi, tx, rx, len, 0);
    if (rc && !s->lastRc) {s->lastRc = rc; s->lastLen = static_cast<uint32_t>(len); s->xIrq = s->spi.snap_irq; s->xGeni = s->spi.snap_status;}
    return rc;
}
void busDelay(void *, unsigned ms) {IOSleep(ms);}

void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}

void rows(const Session &s) {
    const uint32_t *v = s.vals;
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
        "ETCH s%u/%u st%u e%d vote%08x h%08x %08x %08x %08x %08x pll%08x dfs%08x",
        native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
        "ETCH pin%08x %08x %08x %08x irq%08x/%08x/%08x qup%08x fw%08x if%08x L%u/%u %uHz sm%d/%u/%u",
        v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], s.level, s.div, (unsigned)s.actualHz,
        s.smmuRc, s.smmuSlot, s.smmuBank);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
        "ETCH id%02x%02x%02x%02x%02x%02x c%d fw%u/%u v%02x/%02x pr%02x %ux%u max%u,%u pid%04x dl%u/%u rc%d w%02x p%u xf%d/%u",
        s.chip.id[0], s.chip.id[1], s.chip.id[2], s.chip.id[3], s.chip.id[4], s.chip.id[5], s.chip.cascade, s.fw.count,
        static_cast<unsigned>(s.fw.cascade_2nd_header), s.info.fw_ver, s.info.fw_ver_bar, s.info.protocol, s.info.x_num,
        s.info.y_num, s.info.abs_x_max, s.info.abs_y_max, s.info.pid, s.chip.dl_attempt, s.chip.dl_stage, s.chip.dl_rc,
        s.chip.dl_wait_value, s.chip.dl_polls, s.lastRc, s.lastLen);
    char head[25];
    for (unsigned k = 0; k < 12; ++k) snprintf(head + 2 * k, 3, "%02x", s.lastReport[k]);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
        "ETCH irq%u/%u rep%u ok%u crc%u esd%u wdt%u dl%u be%u c%u %d,%d hid%u raw%s",
        s.vals[20], s.vals[21], s.reports, s.ok, s.crc, s.esd, s.wdt, s.downloads, s.busErr, native_touch_contacts,
        native_touch_last_x, native_touch_last_y, native_touch_hid_reports, head);
}
void mark(const Session &s, unsigned step) {native_touch_step = step; rows(s);}

bool snapshot(Session &s) {
    mark(s, 1);
    if (!mapRegion(ETB_GCC_BASE + 0x0000, 0x1000, s.gccLow) || !mapRegion(ETB_GCC_BASE + 0x18000, 0x1000, s.gccS4) ||
        !mapRegion(ETB_GCC_BASE + 0x23000, 0x1000, s.gccHalt) || !mapRegion(ETB_GCC_BASE + 0x52000, 0x1000, s.gccVote) ||
        !mapRegion(ETB_TLMM_BASE + tb_tlmm_ctl(ETB_GPIO_IRQ), 0x1000, s.tlmmIrq) ||
        !mapRegion(ETB_TLMM_BASE + tb_tlmm_ctl(ETB_GPIO_SPI_MISO), 4 * TB_GPIO_STRIDE, s.tlmmSpi)) {
        fail(1, -12);
        return false;
    }
    uint32_t *v = s.vals;
    v[0] = rd32(s.gccVote.va, ETB_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccHalt.va, ETB_HALT_M_AHB & 0xfff);
    v[2] = rd32(s.gccHalt.va, ETB_HALT_S_AHB & 0xfff);
    v[3] = rd32(s.gccHalt.va, ETB_HALT_CORE & 0xfff);
    v[4] = rd32(s.gccHalt.va, ETB_HALT_CORE_2X & 0xfff);
    v[5] = rd32(s.gccS4.va, ETB_HALT_S4 & 0xfff);
    v[6] = rd32(s.gccLow.va, ETB_GCC_GPLL0_MODE);
    v[7] = rd32(s.gccS4.va, (ETB_S4_RCG + TB_RCG_DFSR_CMD) & 0xfff);
    v[8] = rd32(s.gccVote.va, ETB_GCC_GPLL0_ENABLE & 0xfff);
    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        uint32_t cfg = rd32(s.gccS4.va, (ETB_S4_RCG + TB_RCG_PERF_CFG(l)) & 0xfff);
        uint32_t m = rd32(s.gccS4.va, (ETB_S4_RCG + TB_RCG_PERF_M(l)) & 0xfff);
        uint32_t n = rd32(s.gccS4.va, (ETB_S4_RCG + TB_RCG_PERF_N(l)) & 0xfff);
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
    }
    for (unsigned i = 0; i < 4; ++i) v[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    v[13] = rd32(s.tlmmIrq.va, 0);     // CTL
    v[14] = rd32(s.tlmmIrq.va, 4);     // IN_OUT
    v[15] = rd32(s.tlmmIrq.va, 8);     // INTR_CFG
    return true;
}

bool enable(Session &s) {
    mark(s, 2);
    wr32(s.gccVote.va, ETB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, ETB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, ETB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    s.vals[6] = rd32(s.gccLow.va, ETB_GCC_GPLL0_MODE);
    wr32(s.gccVote.va, ETB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, ETB_GCC_VOTE_ENABLE & 0xfff) | ETB_VOTE_ALL);
    // Branches with hardware clock gating (bit 1) show CLK_OFF while idle and wake on access.
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccHalt.va, ETB_HALT_M_AHB & 0xfff)) && running(rd32(s.gccHalt.va, ETB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccHalt.va, ETB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccHalt.va, ETB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccS4.va, ETB_HALT_S4 & 0xfff) & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.vals[0] = rd32(s.gccVote.va, ETB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[1] = rd32(s.gccHalt.va, ETB_HALT_M_AHB & 0xfff);
    s.vals[2] = rd32(s.gccHalt.va, ETB_HALT_S_AHB & 0xfff);
    s.vals[3] = rd32(s.gccHalt.va, ETB_HALT_CORE & 0xfff);
    s.vals[4] = rd32(s.gccHalt.va, ETB_HALT_CORE_2X & 0xfff);
    s.vals[5] = rd32(s.gccS4.va, ETB_HALT_S4 & 0xfff);
    if (!ok) {fail(2, -110); return false;}   // never touch the engine without running clocks
    const uint32_t cfg = tb_pin_cfg(ETB_FUNC_QUP1_SE4, ETB_DRIVE_6MA);
    for (unsigned i = 0; i < 4; ++i) wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, cfg);
    // The interrupt pin as a plain input (function 0, 8 mA, no pull, output disabled), independent of what the firmware left.
    wr32(s.tlmmIrq.va, 0, tb_pin_cfg(0, ETB_DRIVE_8MA));
    for (unsigned i = 0; i < 4; ++i) s.vals[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    s.vals[13] = rd32(s.tlmmIrq.va, 0);
    mark(s, 3);   // the next accesses touch the engine: if the page freezes here, the engine access hung
    if (!mapRegion(ETB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(ETB_SE_BASE, ETB_SE_SIZE, s.se)) {fail(3, -12); return false;}
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    mark(s, 31);
    int r = geni_spi_read_info(&s.spi);
    s.vals[16] = s.spi.info.qup_hw_version;
    s.vals[17] = s.spi.info.fw_revision;
    s.vals[18] = s.spi.info.if_disable;
    if (r) {fail(3, r); return false;}
    return true;
}

// SE DMA for an engine whose FIFO interface is disabled. The buffers are reached through the UFS identity stage-1 context:
// on SM8750 a pass-through bank stalls DMA silently (drivers/NativeUfs, native-ufs-smmu=2).
bool prepareDma(Session &s) {
    mark(s, 41);
    s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), 2 * kDmaBytes, 0xfffffff000ULL);
    if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) {fail(41, -12); return false;}
    addr64_t segLen = 0;
    s.dmaPhys = s.dmaMem->getPhysicalSegment(0, &segLen);
    uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
    if (!virt || !s.dmaPhys || segLen < 2 * kDmaBytes) {fail(42, -14); return false;}
    memset(virt, 0, 2 * kDmaBytes);
    cacheRange(virt, 2 * kDmaBytes, 1);
    mark(s, 43);
    // The display's late probe can start this thread before NativeUfs has adopted the controller and programmed its bank (hardware
    // 2026-10-23: -1 at the first try), so wait for it.
    s.smmuRc = -99;
    for (unsigned i = 0; i < 120 && &native_ufs_smmu_share; ++i) {
        s.smmuRc = native_ufs_smmu_share(ETB_QUP_SID, &s.smmuSlot, &s.smmuBank);
        if (s.smmuRc != -1) break;
        IOSleep(500);
    }
    if (s.smmuRc) {fail(43, -100 + s.smmuRc); return false;}
    s.spi.use_dma = 1;
    s.spi.dma.ctx = nullptr;
    s.spi.dma.clean = dmaClean;
    s.spi.dma.clean_inval = dmaCleanInval;
    s.spi.dma.inval = dmaInval;
    s.spi.dma.tx_virt = virt;
    s.spi.dma.rx_virt = virt + kDmaBytes;
    s.spi.dma.tx_phys = s.dmaPhys;
    s.spi.dma.rx_phys = s.dmaPhys + kDmaBytes;
    s.spi.dma.size = kDmaBytes;
    return true;
}

bool openBus(Session &s, unsigned hz) {
    mark(s, 4);
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 2;
    if (s.spi.info.fifo_disabled && !prepareDma(s)) return false;
    int r = geni_spi_init(&s.spi);
    if (r) {fail(4, r); return false;}
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, TB_DFS_LEVELS, s.spi.oversampling, hz, &choice)) {fail(5, -22); return false;}
    s.level = choice.level; s.div = choice.div; s.actualHz = choice.actual_hz;
    r = geni_spi_set_clock(&s.spi, hz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (!r) r = geni_spi_setup(&s.spi, ETB_SPI_MODE, 0);
    if (r) {fail(5, r); return false;}
    return true;
}

// Firmware download (also after a watchdog report: the in-cell controller loses its RAM image with the panel).
bool download(Session &s) {
    mark(s, 9);
    s.chip.reset_retries = 60;
    int r = nvt_download_firmware(&s.chip, &s.fw);
    ++s.downloads;
    if (r) {fail(9, r); rows(s); return false;}
    mark(s, 10);
    r = nt36536_get_info(&s.chip, &s.info);
    if (r == NVT_ERR_BUS) {fail(10, r); return false;}
    return true;
}

// Firmware coordinates to TouchProbe's published convention. The firmware's info block names its axis maxima; the longer one is
// the panel's 3392-px axis (hardware 2026-10-23: abs_x_max 33920, abs_y_max 24000, so x runs along the long axis, unlike the DT's
// portrait panel-coords). erhai-touch-orient: bit 0 mirrors the long axis, bit 1 the short axis (from a photographed test).
unsigned gOrient;
void toPublished(const Session &s, struct nvt_contact &c) {
    const int xMax = s.info.abs_x_max >= 1000 ? s.info.abs_x_max : ETB_FW_Y_MAX;
    const int yMax = s.info.abs_y_max >= 1000 ? s.info.abs_y_max : ETB_FW_X_MAX;
    int fx = c.x < 0 ? 0 : (c.x >= xMax ? xMax - 1 : c.x), fy = c.y < 0 ? 0 : (c.y >= yMax ? yMax - 1 : c.y);
    const bool xLong = xMax >= yMax;
    int longAxis = xLong ? (int)((int64_t)fx * ETB_PUB_X_MAX / xMax) : (int)((int64_t)fy * ETB_PUB_X_MAX / yMax);
    int shortAxis = xLong ? (int)((int64_t)fy * ETB_PUB_Y_MAX / yMax) : (int)((int64_t)fx * ETB_PUB_Y_MAX / xMax);
    if (gOrient & 1) longAxis = ETB_PUB_X_MAX - 1 - longAxis;
    if (gOrient & 2) shortAxis = ETB_PUB_Y_MAX - 1 - shortAxis;
    c.x = longAxis;
    c.y = ETB_PUB_Y_MAX - 1 - shortAxis;   // publish() mirrors y once more
}
}  // namespace

// The touch interrupt: GPIO 162, falling edge, through the TLMM summary interrupt (SPI 208). pinctrl-sm8750.c: INTR_CFG bit 0 enable,
// bit 1 polarity, bits 3:2 detection, bit 4 raw status, bits 6/7 wakeup present/enable, bits 10:8 target (3 = applications processor);
// INTR_STATUS bit 0, acknowledged by writing 0.
static volatile uint32_t gIrqEvents, gIrqSpurious;
static volatile uint32_t *gPin;
static void touchIrq(void *) {
    if (!(gPin[0xc / 4] & 1)) {gIrqSpurious = gIrqSpurious + 1; return;}
    gPin[0xc / 4] = 0;
    __atomic_fetch_add(&gIrqEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gIrqEvents);
}
static bool armIrq(Session &s) {
    gPin = reinterpret_cast<volatile uint32_t *>(s.tlmmIrq.va);
    uint32_t cfg = gPin[0x8 / 4];
    cfg &= ~((0x3fu << 5) | (3u << 2) | (1u << 1) | 1u);
    cfg |= (3u << 8) | (2u << 2) | (1u << 1) | (1u << 4);   // applications processor, falling edge, raw status on
    gPin[0x8 / 4] = cfg;
    gPin[0xc / 4] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    s.vals[15] = cfg;
    if (!armcpu_spi_attach) return false;
    if (armcpu_spi_attach(32 + ETB_TLMM_SUMMARY_SPI, false, touchIrq, nullptr, nullptr, nullptr)) return false;
    gPin[0x8 / 4] = cfg | 1u;   // enable last
    s.vals[15] = gPin[0x8 / 4];
    return true;
}

static void touchThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "erhai-touch");
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    const unsigned stage = native_touch_stage_req;
    unsigned hz = ETB_SPI_HZ;
    PE_parse_boot_argn("erhai-touch-hz", &hz, sizeof hz);
    if (hz < 100000 || hz > ETB_SPI_HZ) hz = ETB_SPI_HZ;
    PE_parse_boot_argn("erhai-touch-orient", &gOrient, sizeof gOrient);
    do {
        if (!snapshot(*s)) break;
        native_touch_stage_done = 1;
        rows(*s);
        if (stage < 2) break;
        if (!enable(*s)) break;
        native_touch_stage_done = 2;
        rows(*s);
        if (stage < 3) break;
        if (!openBus(*s, hz)) break;
        s->tx = static_cast<uint8_t *>(IOMalloc(NVT_TRANSFER_LEN + 8));
        s->rx = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 2));
        if (!s->tx || !s->rx) {fail(6, -12); break;}
        s->bus = (struct nvt_bus){s, busXfer, busDelay};
        s->chip = {};
        s->chip.bus = &s->bus;
        s->chip.scratch = s->tx;
        s->chip.rxbuf = s->rx;
        mark(*s, 7);
        int r = nt36536_identify(&s->chip);
        if (r) {fail(7, r); break;}
        native_touch_stage_done = 3;
        rows(*s);
        if (stage < 4) break;
        if (!&erhai_touch_firmware || !&erhai_touch_firmware_size || !erhai_touch_firmware_size) {fail(8, -2); break;}
        mark(*s, 8);
        r = nt36536_fw_parse(&s->fw, erhai_touch_firmware, erhai_touch_firmware_size);
        if (r) {fail(8, r); break;}
        if (!download(*s)) break;
        native_touch_stage_done = 4;
        mark(*s, 11);
        unsigned poll = 0;
        PE_parse_boot_argn("erhai-touch-poll", &poll, sizeof poll);
        const bool irq = !poll && armIrq(*s);
        s->vals[20] = irq;
        rows(*s);
        uint8_t report[NT36536_REPORT_LEN];
        uint32_t seen = gIrqEvents;
        unsigned published = 0, tick = 0;
        for (;;) {
            if (irq) {
                assert_wait((event_t)&gIrqEvents, THREAD_UNINT);
                if (gIrqEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
                else thread_block(THREAD_CONTINUE_NULL);
                seen = gIrqEvents;
            } else {
                IOSleep(8);
            }
            s->vals[21] = gIrqEvents;
            if (nt36536_read_report(&s->chip, report)) {++s->busErr; if ((++tick & 15) == 0) rows(*s); continue;}
            ++s->reports;
            memcpy(s->lastReport, report + 1, sizeof s->lastReport);
            struct nvt_contact c[NT36536_POINTS];
            unsigned n = 0;
            r = nt36536_decode(report, s->info.protocol == NT36536_HIRES, c, NT36536_POINTS, &n);
            if (r == NT36536_REPORT_ESD) {++s->esd; rows(*s); continue;}
            if (r == NT36536_REPORT_WDT) {++s->wdt; if (!download(*s)) break; rows(*s); continue;}
            if (r == NT36536_REPORT_CHECKSUM) {++s->crc; if ((++tick & 15) == 0) rows(*s); continue;}
            ++s->ok;
            native_touch_frames = native_touch_frames + 1;
            if (n || published) {
                for (unsigned i = 0; i < n; ++i) toPublished(*s, c[i]);
                native_touch_contacts = n;
                if (n) {native_touch_last_x = c[0].x; native_touch_last_y = ETB_PUB_Y_MAX - 1 - c[0].y;}
                native_touch_valid = native_touch_valid + 1;
                native_touch_publish(n, c);
                published = n;
            }
            if ((++tick & 15) == 0) rows(*s);
        }
    } while (0);
    rows(*s);
}

// Called from the display's late probe (LegacyDisplayProbe.cpp), beside sheng's native_touch_probe. Shares its status rows and
// stage counters, so sheng-touch and erhai-touch must not both be set.
extern "C" void erhai_touch_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("erhai-touch", &stage, sizeof stage) || stage < 1 || native_touch_stage_req) return;
    if (stage > 4) stage = 4;
    native_touch_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_touch_error = -12;
}
