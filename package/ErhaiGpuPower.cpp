// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off, staged observation of the erhai (SM8750) Adreno 830 power state and of the adreno SMMU's stream routing: S0 of
// research/a830-gpu-plan.md. The sequence and its gating live in ErhaiGpuBoard.h (host-tested); this file maps the windows,
// runs it in a thread about ten seconds after the display is up and draws the result. `erhai-gpu-power=<n>`:
//   1  read only: GCC GPU branches, GPUCC CX GDSC and branches, the speed-bin fuse word and the command-DB header; the gxclkctl GX
//      GDSC and the adreno SMMU (global IDs, every valid SMR/S2CR, the banks of SIDs 0/1/2/5/7) only if CX is already on
//   2  as 1, plus the one write the gcc-sm8750 probe makes: enable gcc_gpu_cfg_ahb_clk if its enable bit is clear
//   3  as 2, plus, if CX is off: gdsc.c's init and enable sequence for gpu_cc_cx_gdsc and the gpu_cc_ahb_clk /
//      gpu_cc_hlos1_vote_gpu_smmu_clk enables, so the SMMU global space can be read. GX and the SMMU are never written.
//   4  as 3, then, only if the SMMU read cleanly with CX on and no stream-match entry valid: S1, the GMU's own context through
//      NativeAdreno (NativeAdrenoGmuContextBringup, built for erhai by build-erhai-adreno-context.py): the GMU payloads at their
//      fixed a6xx_gmu.c / gen8 addresses, native 4K TTBR0 tables, context bank 1 / ASID 1, SMR+S2CR slot 2 for the GMU stream 5
//      (the live DTB's gmu iommus), the same choices as sheng's mode 4; slot 2's S2CR is first set to FAULT, as Linux's SMMU
//      reset does for every slot (firmware leaves unused ones translating to a nonexistent bank, which the kit refuses). The GMU stays in reset, so no transaction uses the
//      context yet: this SMMU has no translation operations (ID0.ATOSNS), so the check is the kit's register readback plus a
//      software walk of each payload's first page through the bank's TTBR0, compared with the allocator's physical address.
// Rows: diagnostic extension slots 4-7 and the two wide rows (the banner's GPU3-GPU6 and GPU7-GPU8, shown with sheng-banner=1).
// `erhai-gpu-dump=1` additionally panics with the whole report (every valid stream-match entry), one photo of the panic screen,
// and starts a watchdog that panics with the stage reached if the probe has not finished 70 s after it started.
// sheng-gpu-power must stay unset: GpuPowerProbe uses the same rows with SM8550's offsets.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "ErhaiGpuBoard.h"
#include "NativeAdrenoGmuContext.h"

extern "C" {
extern char boot_progress_ext[8][48];
extern char boot_progress_ext_wide[2][112];
struct egb_result erhai_gpu_power_result;
volatile uint32_t erhai_gpu_power_stage;
}

namespace {
constexpr uint64_t kPhys[EGB_WINDOWS] = {EGB_GCC_GPU_PAGE, EGB_GCC_VOTE_PAGE, EGB_GPUCC_BASE, EGB_GXCLK_PAGE, EGB_SMMU_BASE,
                                         EGB_FUSE_PAGE, EGB_CMDDB_BASE};
constexpr uint64_t kLen[EGB_WINDOWS] = {0x1000, 0x1000, 0xa000, 0x1000, EGB_SMMU_SIZE, 0x1000, 0x1000};
constexpr uint64_t kReadable[EGB_WINDOWS] = {0x1000, 0x1000, EGB_GPUCC_SIZE, 0x1000, EGB_SMMU_SIZE, 0x1000, 0x1000};
OSSharedPtr<IOMemoryMap> gMaps[EGB_WINDOWS];
uintptr_t gVa[EGB_WINDOWS];
unsigned gMode;

// Each access first names itself in slot 4, so an access that stalls the bus (no abort, the thread just stops) stays on screen.
void setRow(unsigned slot, const char *text);
void mark(char op, unsigned w, uint32_t off) {
    char row[48];
    snprintf(row, sizeof row, "EG m%u %c w%u+%05x", gMode, op, w, off);
    setRow(4, row);
}
int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w >= EGB_WINDOWS || !gVa[w] || off >= kReadable[w] || (off & 3)) return -1;
    mark('r', w, off);
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}
// The only writes: the GCC cfg AHB enable (mode 2) and the CX GDSC / two branch enables (mode 3).
int wr(void *, unsigned w, uint32_t off, uint32_t value) {
    const bool gcc = w == EGB_WIN_GCC_GPU && off == EGB_GCC_GPU_CFG_AHB;
    const bool cc = w == EGB_WIN_GPUCC && (off == EGB_CC_CX_GDSCR || off == EGB_CC_AHB_CLK || off == EGB_CC_SMMU_VOTE_CLK);
    if (w >= EGB_WINDOWS || !gVa[w] || off >= kReadable[w] || (off & 3)) return -1;
    if (!((gcc && gMode >= 2) || (cc && gMode >= 3))) return -1;
    mark('w', w, off);
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
void delayUs(void *, unsigned us) {IODelay(us);}
void setRow(unsigned slot, const char *text) {
    unsigned i = 0;
    for (; i < 47 && text[i]; ++i) boot_progress_ext[slot][i] = text[i];
    for (; i < 47; ++i) boot_progress_ext[slot][i] = ' ';
    boot_progress_ext[slot][47] = 0;
}
// A stream's routing in four characters: "-" no valid SMR, "f" fault, "b" bypass, else the bank.
void sidText(const egb_result &r, unsigned s, char *out, size_t size) {
    if (r.sid_slot[s] == EGB_NO_MATCH) snprintf(out, size, "-");
    else if (r.sid_type[s] != 0) snprintf(out, size, "%u%c", r.sid_slot[s], r.sid_type[s] == 1 ? 'b' : 'f');
    else snprintf(out, size, "%u>%u", r.sid_slot[s], r.sid_cb[s]);
}
void publish(const egb_result &r, unsigned have) {
    char row[120];
    snprintf(row, sizeof row, "EG m%u w%02x gcc=%s cc=%s gx=%s sm=%s io%d p%u%u%u", r.mode, have, egb_state_text(r.gcc),
             egb_state_text(r.cc), egb_state_text(r.gx), egb_state_text(r.smmu), r.io_error, r.enabled_ahb, r.powered_cx,
             r.powered_clocks);
    setRow(4, row);
    snprintf(row, sizeof row, "ahb=%08x>%08x gem=%08x axi=%08x", r.cfg_ahb, r.cfg_ahb_after, r.gemnoc, r.axi);
    setRow(5, row);
    snprintf(row, sizeof row, "cx=%08x>%08x hw=%08x sv=%08x", r.cx_before, r.cx_gdscr, r.cx_hwctrl, r.cc_smmu);
    setRow(6, row);
    snprintf(row, sizeof row, "gx=%08x/%08x a=%08x bin=%03x db%u", r.gx_gdscr, r.gx_cfg, r.cc_ahb, r.speed_bin_word & 0x1ff,
             r.cmddb_magic == EGB_CMDDB_MAGIC ? r.cmddb_version : 0xeeu);
    setRow(7, row);
    char sid[EGB_SIDS][8];
    for (unsigned s = 0; s < EGB_SIDS; ++s) sidText(r, s, sid[s], sizeof sid[s]);
    snprintf(boot_progress_ext_wide[0], sizeof boot_progress_ext_wide[0],
             "SM s0=%08x i0=%08x i1=%08x i2=%08x i7=%08x gf=%08x v%u/%u cb%u t%u/%u/%u sid0:%s 1:%s 2:%s 5:%s 7:%s",
             r.scr0, r.id0, r.id1, r.id2, r.id7, r.gfsr, r.smr_valid, r.num_smr, r.num_cb, r.types[0], r.types[1], r.types[2],
             sid[0], sid[1], sid[2], sid[3], sid[4]);
    size_t at = size_t(snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1], "SMR"));
    for (unsigned i = 0; i < r.shown && at < 100; ++i)
        at += (size_t)snprintf(boot_progress_ext_wide[1] + at, sizeof boot_progress_ext_wide[1] - at, " %u:%x/%x", r.smr[i].index,
                       r.smr[i].smr & 0x7fffffffu, r.smr[i].s2cr);
}
// S1. Reads one 64-bit word of physical memory through a fresh uncached mapping: the table as DRAM holds it, which is what
// the SMMU walker reads (GpuPowerProbe's physRead64; not IOMemoryDescriptor::map, which changes the page's own cache mode).
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t size);
NativeAdrenoGmuContextReport gContext;
IOReturn gContextStatus = kIOReturnNotReady;
unsigned gWalked, gWalkOk;
volatile bool gFinished;
uint32_t gS2crBefore;
uint64_t gWalkBad;
bool physRead64(uint64_t pa, uint64_t *out) {
    const vm_offset_t va = ml_io_map_unmappable(vm_offset_t(pa & ~uint64_t(0xfff)), 0x1000, 0x7 /* VM_WIMG_IO */);
    if (!va) return false;
    *out = *reinterpret_cast<volatile uint64_t *>(va + (pa & 0xff8));
    ml_io_unmap(va, 0x1000);
    return true;
}
// Walk one address through context bank `bank`'s TTBR0 (TCR T0SZ, 4 KiB granule); the physical address it maps to, 0 if none.
uint64_t walk(unsigned bank, uint64_t va) {
    const uint32_t id1 = gContext.id1;
    const uint32_t page = egb_smmu_page(id1), cb = egb_smmu_cb_base(id1) + bank * page;
    if (!gVa[EGB_WIN_SMMU] || cb + page > EGB_SMMU_SIZE) return 0;
    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(gVa[EGB_WIN_SMMU]);
    const uint64_t ttbr = (uint64_t(base[(cb + 0x24) / 4]) << 32) | base[(cb + 0x20) / 4];
    const unsigned bits = 64 - (base[(cb + 0x30) / 4] & 0x3f);
    if (bits < 25 || bits > 48) return 0;
    uint64_t table = ttbr & 0x0000fffffffff000ull, desc = 0;
    for (int level = 4 - int((bits - 12 + 8) / 9); level <= 3; ++level) {
        const unsigned shift = 12 + 9 * unsigned(3 - level);
        if (!physRead64(table + ((va >> shift) & 0x1ff) * 8, &desc) || !(desc & 1)) return 0;
        if (level == 3 || !(desc & 2))
            return (desc & 0x0000fffffffff000ull & ~((uint64_t(1) << shift) - 1)) | (va & ((uint64_t(1) << shift) - 1));
        table = desc & 0x0000fffffffff000ull;
    }
    return 0;
}
void contextProgress(uint32_t step) {
    char row[48];
    snprintf(row, sizeof row, "GMUCTX step %u running", step);
    setRow(5, row);
    erhai_gpu_power_stage = 0x40 + step;
}
constexpr unsigned kGmuBank = 1, kGmuAsid = 1, kGmuSlot = 2, kGmuSid = 5;
void gmuContext(const egb_result &r) {
    char row[64];
    if (r.smmu != EGB_DONE || r.io_error || r.smr_valid) {setRow(5, "GMUCTX skip prereq"); return;}
    // The kit leases only a slot whose S2CR is FAULT; firmware leaves unused ones translating to a bank that does not
    // exist (0x03000018). As Linux's arm_smmu_device_reset does for every slot, fault ours first (its SMR is invalid, so
    // the S2CR is inert until the attach installs the SID).
    volatile uint32_t *smmu = reinterpret_cast<volatile uint32_t *>(gVa[EGB_WIN_SMMU]);
    gS2crBefore = smmu[(0xc00 + 4 * kGmuSlot) / 4];
    if (smmu[(0x800 + 4 * kGmuSlot) / 4] & 0x80000000u) {setRow(5, "GMUCTX skip slot valid"); return;}
    smmu[(0xc00 + 4 * kGmuSlot) / 4] = 0x20000u;
    __asm__ volatile("dsb sy" ::: "memory");
    if (smmu[(0xc00 + 4 * kGmuSlot) / 4] != 0x20000u) {setRow(5, "GMUCTX skip s2cr fault"); return;}
    gContextStatus = NativeAdrenoGmuContextBringup(reinterpret_cast<volatile uint8_t *>(gVa[EGB_WIN_SMMU]), EGB_SMMU_SIZE,
                                                   kGmuBank, kGmuAsid, kGmuSlot, kGmuSid, contextProgress, &gContext);
    const NativeAdrenoGmuContextReport &g = gContext;
    // B s<step> st<IOReturn> k<kit program status>/<attach status> p<phase>w<writes>r<readback>e<enabled> r<route phase>v<verified>
    snprintf(row, sizeof row, "B s%u st%x k%u/%u p%uw%ur%ue%u r%uv%u m%x", g.step, unsigned(gContextStatus), g.program_status,
             g.route_status, g.program_phase, g.program_writes, g.program_readback_valid, g.program_enabled, g.route_phase,
             g.routes_verified, g.mismatch);
    setRow(5, row);
    snprintf(row, sizeof row, "T%llx S%x C%x pre%x", static_cast<unsigned long long>(g.ttbr0), g.sctlr, g.cbar, gS2crBefore);
    setRow(6, row);
    // The walk: each payload's first and last page must map to the allocator's physical pages.
    NativeAdrenoGmuPayload payloads[5];
    if (!gContextStatus && NativeAdrenoGmuContextPayloads(payloads) == kIOReturnSuccess) {
        for (unsigned i = 0; i < 5; ++i) {
            const uint64_t probes[2] = {0, (payloads[i].bytes - 1) & ~uint64_t(0xfff)};
            for (uint64_t offset : probes) {
                ++gWalked;
                const uint64_t pa = walk(kGmuBank, payloads[i].iova + offset);
                if (pa == payloads[i].physical + offset) ++gWalkOk;
                else if (!gWalkBad) gWalkBad = payloads[i].iova + offset;
            }
        }
    }
    snprintf(row, sizeof row, "R%08x S2%08x walk %u/%u bad=%llx", g.smr, g.s2cr, gWalkOk, gWalked,
             static_cast<unsigned long long>(gWalkBad));
    setRow(7, row);
    const uint32_t gfsr = *reinterpret_cast<volatile uint32_t *>(gVa[EGB_WIN_SMMU] + EGB_SMMU_GFSR);
    snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1],
             "S1 bank%u asid%u slot%u sid%u out%u spans%u id1=%08x gfsr=%08x a2=%x tcr=%x t2=%x mair=%x/%x", kGmuBank, kGmuAsid,
             kGmuSlot, kGmuSid, g.output_bits, g.spans, g.id1, gfsr, g.image.cba2r, g.image.tcr, g.image.tcr2, g.image.mair0,
             g.image.mair1);
    erhai_gpu_power_stage = gContextStatus ? 0x4f : 0x47;
}
// The whole report for the panic screen: every shown SMR, and each DT stream's bank registers.
void dump(const egb_result &r) {
    static char text[2400];
    size_t at = (size_t)snprintf(text, sizeof text, "ERHAI_GPU m%u\n%.47s\n%.47s\n%.47s\n%.47s\n%.111s\n%.111s\n", gMode, boot_progress_ext[4],
                      boot_progress_ext[5], boot_progress_ext[6], boot_progress_ext[7], boot_progress_ext_wide[0],
                      boot_progress_ext_wide[1]);
    at += (size_t)snprintf(text + at, sizeof text - at, "cc gmu=%08x cxo=%08x hub=%08x gxgmu=%08x gxbcr=%08x vote=%08x fuse=%08x\n",
                   r.cc_gmu, r.cc_cxo, r.cc_hub, r.cc_gx_gmu, r.gx_bcr, r.pll_vote, r.speed_bin_word);
    for (unsigned i = 0; i < r.shown && at < sizeof text - 200; ++i)
        at += (size_t)snprintf(text + at, sizeof text - at, "%u:%08x/%08x%s", r.smr[i].index, r.smr[i].smr, r.smr[i].s2cr,
                       (i % 4 == 3) ? "\n" : "  ");
    at += (size_t)snprintf(text + at, sizeof text - at, "\n");
    for (unsigned s = 0; s < EGB_SIDS && at < sizeof text - 100; ++s)
        at += (size_t)snprintf(text + at, sizeof text - at, "sid%u slot=%u cb=%u type=%u cbar=%08x sctlr=%08x\n", egb_sids[s],
                       r.sid_slot[s], r.sid_cb[s], r.sid_type[s], r.sid_cbar[s], r.sid_sctlr[s]);
    panic("%s", text);
}
void probeThread(void *, wait_result_t) {
    IOSleep(10000);
    erhai_gpu_power_stage = 2;
    unsigned have = 0;
    for (unsigned w = 0; w < EGB_WINDOWS; ++w) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kPhys[w], kLen[w]);
        if (!memory) continue;
        gMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
        if (gMaps[w]) {gVa[w] = static_cast<uintptr_t>(gMaps[w]->getVirtualAddress()); have |= 1u << w;}
    }
    // The row first shows "running", so a bus fault inside the sequence still leaves the stage on screen.
    char row[64];
    snprintf(row, sizeof row, "EG m%u w%02x running", gMode, have);
    setRow(4, row);
    erhai_gpu_power_stage = 3;
    egb_io io{nullptr, rd, wr, delayUs};
    egb_run(&io, gMode > 3 ? 3 : gMode, have, &erhai_gpu_power_result);
    publish(erhai_gpu_power_result, have);
    erhai_gpu_power_stage = 4;
    if (gMode >= 4) gmuContext(erhai_gpu_power_result);
    IOLog("ERHAI_GPU %.47s | %.47s | %.47s | %.47s\nERHAI_GPU %.111s\nERHAI_GPU %.111s\n", boot_progress_ext[4],
          boot_progress_ext[5], boot_progress_ext[6], boot_progress_ext[7], boot_progress_ext_wide[0], boot_progress_ext_wide[1]);
    unsigned dumpArg = 0;
    gFinished = true;
    if (PE_parse_boot_argn("erhai-gpu-dump", &dumpArg, sizeof dumpArg) && dumpArg) dump(erhai_gpu_power_result);
}
// With erhai-gpu-dump, a probe that stops without faulting (a stalled access or a wait that never ends) would otherwise leave
// nothing visible once iOS owns the display: after a minute, panic with the stage and the rows written so far.
void watchdogThread(void *, wait_result_t) {
    IOSleep(70000);
    if (gFinished) return;
    panic("ERHAI_GPU WATCHDOG m%u stage=%x\n%.47s\n%.47s\n%.47s\n%.47s\n%.111s\n%.111s\n", gMode, erhai_gpu_power_stage,
          boot_progress_ext[4], boot_progress_ext[5], boot_progress_ext[6], boot_progress_ext[7], boot_progress_ext_wide[0],
          boot_progress_ext_wide[1]);
}
}

extern "C" void erhai_gpu_power_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("erhai-gpu-power", &mode, sizeof mode) || !mode || erhai_gpu_power_stage) return;
    gMode = mode > 4 ? 4 : mode;
    erhai_gpu_power_stage = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else erhai_gpu_power_stage = 0xdead;
    unsigned dumpArg = 0;
    if (PE_parse_boot_argn("erhai-gpu-dump", &dumpArg, sizeof dumpArg) && dumpArg &&
        kernel_thread_start(watchdogThread, nullptr, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
