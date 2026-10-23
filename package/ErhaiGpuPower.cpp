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
//   5  as 4, then, only if the GMU context reached Ready, read-only S2 prerequisites (no write): the GMU clock tree in gpucc
//      (gpu_cc_pll0, gmu/hub RCGs, branches) in rows 6/7; the AOP command DB copied and turned into the A8xx HFI startup
//      table (ShengGmuCommandDbInputs + ShengGmuBuildStartup of the gen8 profile, for this speed-bin fuse, ACD and IFPC off)
//      in wide row 0; the GMU reset/firmware/HFI/power words, only when gpu_cc_cx_gmu_clk runs, in wide row 1. Row 5 keeps
//      the S1 result in short form.
//   6  as 5, plus the GMU clock bring-up of a6xx_gmu_resume (gmuClocksOn): GPLL0 GPU vote, gpu_cc_pll0 configured and locked
//      if not running, gmu_clk_src -> PLL0 even 500 MHz, hub_clk_src -> GPLL0 / 3 200 MHz. Row 7 shows the step reached, the
//      failure code, the lock time and the vote / PLL mode / RCG CFG words before; row 6 the clock tree after. GMU stays in reset.
//   7  as 6, then, only if the clocks came up, the command DB gave a startup table and the GMU reads in reset, idle and
//      without retention: the cold start (gmuColdStart): SCM gpu_init_regs, AO interrupt clear/mask, a write-combine CPU
//      view of the five payloads, ShengGmuColdStart with the embedded gen80000_gmu.bin (hash-checked before any write) and
//      the command-DB table, then GMU2HOST clear/mask. Rows 5-7 and wide row 1 show the SCM, HFI, load and GMU results.
//   8  as 7, then, only after a complete cold start, S3 (gxOn): the HFI GX_BW_PERF_VOTE at `erhai-gpu-perf` (default the
//      lowest OPP) and OOB GPU_SET, so the GMU powers GX; RBBM_STATUS and the GBIF halts are read only if GX reports on.
//      Row 4 shows the result, wide row 0 also the gxclkctl GX GDSCR before > after. No GPU register is written.
//   9  as 8, then, only when GX reports on, S4 (gpuStart): the rest of a8xx hw_init. The GPU's own native-4K context through
//      NativeAdreno (NativeAdrenoGpuContextBringup: bank 0, ASID 2, SIDs 0/1 on SMR slots 0/1, their S2CRs faulted first as
//      for the GMU's) with the SQE, ring, memptrs, power-up list and scratch buffers; gen80000_sqe.fw minus its first dword;
//      ErhaiGpuHwInit (the register part of hw_init, host-compared with Linux by check-erhai-gpu-hwinit.py) around the
//      gen8 NativeAdreno ring attach and CP_ME_INIT; the zap shader (ErhaiZapLoad: PAS 13 into gpu_microcode_mem
//      0x9b09a000, check-erhai-gpu-zap.py) and CP_SET_SECURE_MODE 0; RB_GC_GMEM_PROTECT, the power-up list, clock gating
//      and the OOB GPU_SET clear; then one CP_MEM_WRITE to the scratch page with its fence. Rows 5-7 and wide row 1 show it.
//  10  as 9, then, only after a complete S4, S5 (gpuSubmitTest): the GPU interrupt (SPI 300 = INTID 332, level) attached
//      through armcpu_spi_attach with NativeAdrenoIrq's a8xx_irq acknowledgement, and one more fence that must raise it; the
//      CP and GMU always-on counters measured against CNTVCT (19.2 MHz); two rounds through NativeAdrenoUserClient's
//      dispatch (two objects, the IB's CP_MEM_WRITEs submitted with the a7xx_submit framing, the fence waited, the target
//      read back, unmap and free; round 2 reuses the IB address), then context bank 0's FSR and the GFSR. Rows 5 and 7.
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
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoMemory.h"
#include "AdrenoPM4.h"
#include "ErhaiGpuHwInit.h"
#include "ErhaiGpuZap.h"
#include "ShengGmuVotes.h"
#include "ShengGmuCommandDb.h"
#include "ShengGmu.h"
#include <libkern/crypto/sha2.h>
#include <string.h>

extern "C" {
// Scheduler entry points of osfmk (kern/sched_prim.h, kern/processor.h) for the CPU binding of the probe and its watchdog.
processor_t thread_bind(processor_t);
processor_t cpu_to_processor(int);
wait_result_t thread_block(thread_continue_t);
int cpu_number(void);
extern uint32_t processor_avail_count;
extern unsigned int processor_count;
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
// S2, mode 5 (read-only). The GMU core window is the upstream DT gmu@3d6c000 (0x32000; register dword offsets from
// a6xx_gmu.xml, A8XX variants, minus the 0x6c000 GMU-GPU delta), RSCC sits at GPU base + 0x50000 on A8xx, cx_misc is the
// gpu "cx_mem" range: the same windows prepare-erhai-gmu.py gives ShengGmuBindMmio.
constexpr uint64_t kGmuPhys[3] = {0x3d6c000, 0x3d50000, 0x3d9e000};
constexpr uint64_t kGmuLen[3] = {0x32000, 0x10000, 0x2000};
constexpr uint32_t kGmuReg(uint32_t dword) {return dword * 4 - 0x6c000;}
OSSharedPtr<IOMemoryMap> gGmuMaps[3];
volatile uint32_t *gGmuVa[3];
uint32_t gmuRead(unsigned w, uint32_t off) {
    char row[48];
    snprintf(row, sizeof row, "EG m%u r g%u+%05x", gMode, w, off);
    setRow(4, row);
    return gGmuVa[w][off / 4];
}
// gpucc clock tree of the GMU (gpucc-sm8750.c): gpu_cc_pll0 (taycan_elu at 0x0: MODE, OPMODE 0x4, L 0x10, USER_CTL 0x18),
// gmu_clk_src RCG 0x9318 (CMD, CFG +4), hub_clk_src 0x93ec, hub_div 0x942c and the GMU branches.
struct GmuClocks {uint32_t pll_mode, pll_opmode, pll_l, pll_user, gmu_cmd, gmu_cfg, hub_cmd, hub_cfg, hub_div;
                  uint32_t cx_gmu, cxo, hub_cx_int, hub_aon, ahb, memnoc_gfx;} gClocks;
void readClocks(GmuClocks &k) {
    const uint32_t offsets[] = {0x0, 0x4, 0x10, 0x18, 0x9318, 0x931c, 0x93ec, 0x93f0, 0x942c,
                                EGB_CC_CX_GMU_CLK, EGB_CC_CXO_CLK, EGB_CC_HUB_CX_INT_CLK, 0x93e8, EGB_CC_AHB_CLK, 0x90f4};
    uint32_t *out = &k.pll_mode;
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) rd(nullptr, EGB_WIN_GPUCC, offsets[i], out + i);
}
// The AOP command DB (reserved-memory aop-cmd-db@81c60000, 0x20000, no-map) copied with aligned reads, parsed by
// ShengGmuCommandDbInputs and turned into the A8xx HFI startup table by ShengGmuBuildStartup for this speed bin
// (ACD and IFPC off). The version passed to the parser is the word just read: a pass shows the layout, not a qualified version.
constexpr uint64_t kCommandDbPhys = 0x81c60000, kCommandDbBytes = 0x20000;
ShengGmuDbStatus gDbStatus = ShengGmuDbArgument;
AdrenoHfiStartupStatus gVoteStatus = AdrenoHfiStartupInvalid;
AdrenoHfiStartupConfig gStartup;
ShengGmuVoteInputs gVoteInputs;
uint8_t *gCommandDb;
uint32_t gDbVersion;
void buildVotes(const egb_result &r) {
    IODeviceMemory *memory = IODeviceMemory::withRange(kCommandDbPhys, kCommandDbBytes);
    if (!memory) return;
    OSSharedPtr<IOMemoryMap> map(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    gCommandDb = static_cast<uint8_t *>(IOMalloc(kCommandDbBytes));
    if (!map || !gCommandDb) return;
    const volatile uint32_t *db = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (size_t i = 0; i < kCommandDbBytes / 4; ++i) reinterpret_cast<uint32_t *>(gCommandDb)[i] = db[i];
    gDbVersion = reinterpret_cast<uint32_t *>(gCommandDb)[0];
    gVoteInputs = ShengGmuVoteInputs{};
    gVoteInputs.chip_id = 0x44050001;
    gVoteInputs.speed_bin_fuse = r.speed_bin_word & 0x1ff;
    gDbStatus = ShengGmuCommandDbInputs(gCommandDb, kCommandDbBytes, gDbVersion, &gVoteInputs);
    if (gDbStatus == ShengGmuDbSuccess) gVoteStatus = ShengGmuBuildStartup(&gVoteInputs, &gStartup);
}
struct GmuState {uint32_t reset, init, hfi, version, pwr, rpmh, ao, info, tcm, busy;} gGmuState;
bool readGmuState(GmuState &s) {
    if (!gGmuVa[0] || !gGmuVa[1] || !gGmuVa[2]) return false;
    s.reset = gmuRead(0, kGmuReg(0x1f800));      // CM3_SYSRESET
    s.init = gmuRead(0, kGmuReg(0x1f81c));       // CM3_FW_INIT_RESULT
    s.hfi = gmuRead(0, kGmuReg(0x1f980));        // HFI_CTRL_STATUS
    s.version = gmuRead(0, kGmuReg(0x1d3f8));    // CORE_FW_VERSION
    s.pwr = gmuRead(0, kGmuReg(0x1f7e8));        // A8XX GMU_PWR_CLK_STATUS (bit 0 GX GDSC off, bit 1 GX clock off)
    s.rpmh = gmuRead(0, kGmuReg(0x1f7e9));       // A8XX RPMH_POWER_STATE
    s.ao = gmuRead(0, kGmuReg(0x23b05));         // AO_HOST_INTERRUPT_STATUS
    s.info = gmuRead(0, kGmuReg(0x1f992));       // GMU2HOST_INTR_INFO
    s.tcm = gmuRead(2, 0x39 * 4);                // CX_MISC_TCM_RET_CNTL
    s.busy = gmuRead(1, 0x101 * 4);              // RSCC_SEQ_BUSY_DRV0
    return true;
}
// S2, mode 6: the GMU clock bring-up of a6xx_gmu_resume, clk_set_rate(gmu 200 MHz) / clk_set_rate(hub 150 MHz) as the
// clock framework resolves them with gpucc-sm8750.c's tables: gmu_clk_src -> 500 MHz from gpu_cc_pll0_out_even (src 2,
// div 1), hub_clk_src -> 200 MHz from GPLL0 main (src 5, div 3). Before the RCG switches, the new parents are enabled:
// gcc_gpu_gpll0_clk_src (GCC vote 0x52000 bit 15), and gpu_cc_pll0 configured (clk_lucid_evo_pll_configure with
// gpu_cc_pll0_config, skipped if already running) and enabled (alpha_pll_lucid_evo_enable). RCG update as clk-rcg2.c:
// CFG src/div/mode/HW_CLK_CTRL replaced, CMD UPDATE, wait for it to clear. The branches are already on (mode 5).
enum {kClkVote = 1, kClkConfigure, kClkLock, kClkOutputs, kClkGmuRcg, kClkHubRcg, kClkDone};
uint32_t gClockStep, gClockFail, gLockUs, gVoteBefore, gVoteAfter;
GmuClocks gClocksBefore;
bool clockWrite(unsigned w, uint32_t off, uint32_t value) {
    if (gMode < 6 || !gVa[w] || off >= kReadable[w] || (off & 3)) return false;
    mark('w', w, off);
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}
uint32_t clockRead(unsigned w, uint32_t off) {uint32_t v = 0; rd(nullptr, w, off, &v); return v;}
bool rcgSwitch(uint32_t cmd, uint32_t cfg) {
    const uint32_t mask = 0x1fu | (7u << 8) | (3u << 12) | (1u << 20);    // hid_width 5, SRC_SEL, MODE, HW_CLK_CTRL
    if (!clockWrite(EGB_WIN_GPUCC, cmd + 4, (clockRead(EGB_WIN_GPUCC, cmd + 4) & ~mask) | cfg)) return false;
    if (!clockWrite(EGB_WIN_GPUCC, cmd, clockRead(EGB_WIN_GPUCC, cmd) | 1u)) return false;   // CMD_UPDATE
    for (unsigned us = 0; us < 500; ++us) {
        if (!(clockRead(EGB_WIN_GPUCC, cmd) & 1u)) return true;
        IODelay(1);
    }
    return false;
}
void gmuClocksOn() {
    gClocksBefore = gClocks;
    gClockStep = kClkVote;
    gVoteBefore = clockRead(EGB_WIN_GCC_VOTE, EGB_GCC_PLL_VOTE);
    if (!(gVoteBefore & (1u << 15)) && !clockWrite(EGB_WIN_GCC_VOTE, EGB_GCC_PLL_VOTE, gVoteBefore | (1u << 15))) {gClockFail = 1; return;}
    gVoteAfter = clockRead(EGB_WIN_GCC_VOTE, EGB_GCC_PLL_VOTE);
    const uint32_t mode = clockRead(EGB_WIN_GPUCC, 0x0), opmode = clockRead(EGB_WIN_GPUCC, 0x4);
    if (!((opmode & 1u) && (mode & 1u))) {                                    // trion_pll_is_enabled
        gClockStep = kClkConfigure;
        // L (0x34 | TRION_PLL_CAL_VAL << 16), ALPHA, CONFIG_CTL/_U/_U1, USER_CTL/_U; zero test values are skipped.
        const uint32_t config[][2] = {{0x10, 0x34u | (0x44u << 16)}, {0x14, 0x1555}, {0x20, 0x19660387}, {0x24, 0x098060a0},
                                      {0x28, 0xb416cb20}, {0x18, 0x00000400}, {0x1c, 0x00000002}};
        for (const auto &c : config) if (!clockWrite(EGB_WIN_GPUCC, c[0], c[1])) {gClockFail = 1; return;}
        if (!clockWrite(EGB_WIN_GPUCC, 0x0, clockRead(EGB_WIN_GPUCC, 0x0) & ~1u) || !clockWrite(EGB_WIN_GPUCC, 0x4, 0) ||
            !clockWrite(EGB_WIN_GPUCC, 0x0, clockRead(EGB_WIN_GPUCC, 0x0) | 4u)) {gClockFail = 1; return;}
        if (clockRead(EGB_WIN_GPUCC, 0x18) & (1u << 25)) {gClockFail = 2; return;}   // FSM vote mode: not this sequence
        gClockStep = kClkLock;
        if (!clockWrite(EGB_WIN_GPUCC, 0x0, clockRead(EGB_WIN_GPUCC, 0x0) | 4u) || !clockWrite(EGB_WIN_GPUCC, 0x4, 1)) {gClockFail = 1; return;}
        for (gLockUs = 0; gLockUs < 1500 && !(clockRead(EGB_WIN_GPUCC, 0x0) & (1u << 31)); ++gLockUs) IODelay(1);
        if (gLockUs >= 1500) {gClockFail = 3; return;}
        gClockStep = kClkOutputs;
        if (!clockWrite(EGB_WIN_GPUCC, 0x18, clockRead(EGB_WIN_GPUCC, 0x18) | 7u) ||
            !clockWrite(EGB_WIN_GPUCC, 0x0, clockRead(EGB_WIN_GPUCC, 0x0) | 1u)) {gClockFail = 1; return;}
    }
    gClockStep = kClkGmuRcg;
    if (!rcgSwitch(0x9318, (2u << 8) | 1u)) {gClockFail = 4; return;}
    gClockStep = kClkHubRcg;
    if (!rcgSwitch(0x93ec, (5u << 8) | 5u)) {gClockFail = 5; return;}
    gClockStep = kClkDone;
}
// S2, mode 7: the rest of a6xx_gmu_resume for a cold boot. a6xx_gmu_secure_init (A8xx): the SIP call
// qcom_scm_gpu_init_regs(ALWAYS_EN_REQ | TSENSE_EN_REQ), then CX_MISC_SW_FUSE_VALUE read back; the AO interrupt clear and
// mask; ShengGmuColdStart (gen8 profile) with gen80000_gmu.bin embedded by build-erhai-adreno-context.py and the
// command-DB startup table: firmware load and readback, CM3 release, FW and HFI ready, the HFI startup messages with their
// ACKs. The GMU fetches its icache/HFI payloads through bank 1, so a passing start is S1's translation proven by hardware.
// After it, GMU2HOST clear and unmask of CM3_FAULT only (A6XX_HFI_IRQ_MASK). No GPU frequency vote (mode 8).
extern "C" const uint8_t erhai_gmu_firmware_image[] __attribute__((weak));
extern "C" const uint64_t erhai_gmu_firmware_image_size __attribute__((weak));
constexpr uint64_t kColdGpuPhys = 0x3d00000, kColdGpuLen = 0x6d000;
OSSharedPtr<IOMemoryMap> gColdGpuMap, gPayloadMaps[5];
ShengGmuMmio gColdMmio;
ShengGmuDevice gColdDevice;
ShengGmuConfig gColdConfig;
ShengGmuStartupResult gColdResult;
ShengGmuStatus gColdStatus = ShengGmuArgument;
int64_t gScmRet = -1;
uint32_t gScmResumes, gFuseBefore, gFuseAfter, gColdStep;
int coldSha256(void *, const void *data, size_t bytes, uint8_t digest[32]) {
    if (!data || !bytes || !digest) return 1;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, bytes);
    SHA256_Final(digest, &ctx);
    return 0;
}
int coldNow(void *, uint64_t *us) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000;
    return 0;
}
int coldWait(void *, unsigned us) {IODelay(us); return 0;}
// SIP SMC (qcom_scm-smc.c): ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, svc << 8 | cmd), x1 arginfo, x2..x5 the arguments
// (up to four are passed directly), x1 back is res.result[0]. QCOM_SCM_INTERRUPTED (1) resumes with x0 = 1 and the returned
// x6; QCOM_SCM_V2_EBUSY (-12) retries after 30 ms, 20 times.
int64_t scmCall(uint64_t fn, uint64_t arginfo, const uint64_t *args, unsigned count, uint32_t &resumes, uint64_t *result = nullptr) {
    uint64_t first = fn, a6 = 0;
    const uint64_t a[4] = {count > 0 ? args[0] : 0, count > 1 ? args[1] : 0, count > 2 ? args[2] : 0, count > 3 ? args[3] : 0};
    for (unsigned busy = 0; resumes < 10000;) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = a[0], x3 asm("x3") = a[1];
        register uint64_t x4 asm("x4") = a[2], x5 asm("x5") = a[3], x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resumes; continue;}
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IOSleep(30); continue;}
        if (result) *result = x1;
        return int64_t(x0);
    }
    return -1000;
}
// qcom_scm_remap_error: the secure world's codes as the errno qcom_scm_call returns.
int64_t scmErrno(int64_t a0) {
    switch (a0) {
    case 0: return 0;
    case -1: return -5;                // QCOM_SCM_ERROR -> -EIO
    case -2: case -3: return -22;      // EINVAL_ARG / EINVAL_ADDR -> -EINVAL
    case -4: return -95;               // EOPNOTSUPP
    case -5: return -12;               // ENOMEM
    case -12: return -16;              // V2_EBUSY -> -EBUSY
    default: return -22;
    }
}
void gmuColdStart() {
    gColdStep = 1;
    if (!erhai_gmu_firmware_image || !&erhai_gmu_firmware_image_size || erhai_gmu_firmware_image_size < 4) return;
    gColdStep = 2;
    volatile uint32_t *cx = gGmuVa[2];
    gFuseBefore = cx[0x400];                                                   // CX_MISC_SW_FUSE_VALUE
    erhai_gpu_power_stage = 0x70;
    setRow(4, "EG m7 scm gpu_init_regs");
    const uint64_t initRegs = (1u << 0) | (1u << 3);
    gScmRet = scmCall(0x42000000u | (0x28u << 8) | 0x01u, 1, &initRegs, 1, gScmResumes);
    gFuseAfter = cx[0x400];
    if (gScmRet) return;
    gColdStep = 3;
    IODeviceMemory *gpu = IODeviceMemory::withRange(kColdGpuPhys, kColdGpuLen);
    if (gpu) {
        gColdGpuMap = OSSharedPtr<IOMemoryMap>(gpu->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        gpu->release();
    }
    if (!gColdGpuMap) return;
    gColdMmio = ShengGmuMmio{};
    gColdMmio.windows[ShengGmuCore] = {reinterpret_cast<volatile unsigned char *>(gGmuVa[0]), kGmuPhys[0], kGmuLen[0]};
    gColdMmio.windows[ShengGmuRscc] = {reinterpret_cast<volatile unsigned char *>(gGmuVa[1]), kGmuPhys[1], kGmuLen[1]};
    gColdMmio.windows[ShengGmuGpu] = {reinterpret_cast<volatile unsigned char *>(gColdGpuMap->getVirtualAddress()), kColdGpuPhys,
                                      kColdGpuLen};
    gColdMmio.windows[ShengGmuCxMisc] = {reinterpret_cast<volatile unsigned char *>(cx), kGmuPhys[2], kGmuLen[2]};
    gColdStep = 4;
    NativeAdrenoGmuPayload payloads[5];
    if (NativeAdrenoGmuContextPayloads(payloads)) return;
    ShengGmuMemory *memory[5] = {&gColdConfig.icache, &gColdConfig.dummy, &gColdConfig.debug, &gColdConfig.log, &gColdConfig.hfi};
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(payloads[i].physical, payloads[i].bytes);
        if (m) {
            gPayloadMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gPayloadMaps[i]) return;
        *memory[i] = {reinterpret_cast<volatile uint32_t *>(gPayloadMaps[i]->getVirtualAddress()), payloads[i].bytes,
                      payloads[i].iova};
    }
    gColdStep = 5;
    ShengGmuDevice &d = gColdDevice;
    d = {};
    if (ShengGmuBindMmio(&gColdMmio, &d.io)) return;
    d.services = {nullptr, coldSha256, coldNow, coldWait};
    gColdConfig.chip_id = 0x44050001;
    gColdConfig.firmware = erhai_gmu_firmware_image;
    gColdConfig.firmware_bytes = static_cast<size_t>(erhai_gmu_firmware_image_size);
    gColdConfig.poll_budget = 200;   // ShengGmu also stops each poll at 10 ms, as gmu_poll_timeout(.., 100, 10000)
    // a6xx_gmu_resume, before a6xx_gmu_fw_start: clear every AO interrupt, unmask only A6XX_GMU_IRQ_MASK (WDOG_BITE,
    // FENCE_ERR, AHB_BUS_ERROR).
    d.io.write32(d.io.cookie, ShengGmuCore, kGmuReg(0x23b04), ~0u);
    d.io.write32(d.io.cookie, ShengGmuCore, kGmuReg(0x23b06), ~((1u << 0) | (1u << 3) | (1u << 5)));
    d.io.barrier(d.io.cookie);
    const ShengGmuStartupLimits limits = {1000, 100, 50000, 2000000};
    gColdStep = 6;
    erhai_gpu_power_stage = 0x72;
    setRow(4, "EG m7 cold start running");
    gColdStatus = ShengGmuColdStart(&d, &gColdConfig, &gStartup, &limits, &gColdResult);
    erhai_gpu_power_stage = gColdStatus ? 0x7f : 0x77;
    if (gColdStatus) return;
    gColdStep = 7;
    // After a6xx_hfi_start: GMU2HOST clear, unmask only CM3_FAULT.
    d.io.write32(d.io.cookie, ShengGmuCore, kGmuReg(0x1f991), ~0u);
    d.io.write32(d.io.cookie, ShengGmuCore, kGmuReg(0x1f993), ~(1u << 23));
    d.io.barrier(d.io.cookie);
}
// S3, mode 8: a6xx_gmu_set_initial_freq, then the first step of a8xx hw_init. The HFI GX_BW_PERF_VOTE (a6xx_hfi_set_freq:
// blocking ack, perf index, bandwidth index; `erhai-gpu-perf=<n>`, default 1 = the lowest real OPP, 160 MHz on this bin, and
// the bandwidth level its opp-peak-kBps names), then OOB GPU_SET (HOST2GMU bit 30, GMU2HOST_INFO ack bit 31, 10 ms at 100 us,
// then cleared), which makes the GMU power GX. A8xx's GX GDSC sits in gxclkctl, which Linux leaves to the GMU (gxpd only for
// recovery): it is read, never written. GX counts as on per a8xx_gmu_gx_is_on (GMU_PWR_CLK_STATUS bits 0 and 1 clear); only
// then RBBM_STATUS and both GBIF halts are read. No GPU register is written.
uint32_t gGx[10];   // vote status, fw error, ack polls (101 = none), pwr before/after, gdscr before/after, RBBM_STATUS, GBIF_HALT, RBBM_GBIF_HALT
unsigned gPerf, gBw;
int hfiWait(void *) {IODelay(100); return 0;}
void gxOn() {
    ShengGmuIo &io = gColdDevice.io;
    auto core = [&](uint32_t dword) {uint32_t v = 0xdeadbeef; io.read32(io.cookie, ShengGmuCore, kGmuReg(dword), &v); return v;};
    auto gpu = [&](uint32_t dword) {uint32_t v = 0xdeadbeef; io.read32(io.cookie, ShengGmuGpu, size_t(dword) * 4, &v); return v;};
    uint32_t *g = gGx;
    g[3] = core(0x1f7e8);
    rd(nullptr, EGB_WIN_GXCLK, EGB_GX_GDSCR, &g[5]);
    gPerf = 1;
    PE_parse_boot_argn("erhai-gpu-perf", &gPerf, sizeof gPerf);
    gPerf = ShengGmuPerfVote(gPerf, &gBw);
    const uint32_t vote[3] = {1, gPerf, gBw};
    uint32_t out[16], error = 0;
    size_t count = 0;
    erhai_gpu_power_stage = 0x81;
    setRow(4, "EG m8 hfi perf vote");
    g[0] = AdrenoHfiExchange(&gColdDevice.hfi, 30, vote, 3, 1000, hfiWait, out, 16, &count, &error);
    g[1] = error;
    g[2] = 101;
    if (!g[0]) {
        erhai_gpu_power_stage = 0x82;
        setRow(4, "EG m8 oob gpu_set");
        io.write32(io.cookie, ShengGmuCore, kGmuReg(0x1f994), 1u << 30);   // HOST2GMU_INTR_SET
        io.barrier(io.cookie);
        for (g[2] = 1; g[2] <= 100; ++g[2]) {
            if (core(0x1f992) & (1u << 31)) break;                          // GMU2HOST_INTR_INFO
            IODelay(100);
        }
        io.write32(io.cookie, ShengGmuCore, kGmuReg(0x1f991), 1u << 31);   // GMU2HOST_INTR_CLR
        io.barrier(io.cookie);
    }
    g[4] = core(0x1f7e8);
    rd(nullptr, EGB_WIN_GXCLK, EGB_GX_GDSCR, &g[6]);
    erhai_gpu_power_stage = 0x83;
    const bool gx = g[2] <= 100 && !(g[4] & 3);
    if (gx) {
        g[7] = gpu(0x012);    // A8XX RBBM_STATUS
        g[8] = gpu(0x3c45);   // GBIF_HALT
        g[9] = gpu(0x0000a);  // A8XX RBBM_GBIF_HALT
    }
    erhai_gpu_power_stage = gx ? 0x87 : 0x8f;
}
// S4, mode 9: the rest of a8xx hw_init after OOB GPU_SET, in Linux order (research/a830-gpu-plan.md S4).
extern "C" IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *, size_t, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, void (*)(uint32_t), NativeAdrenoGmuContextReport *) __attribute__((weak));
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[5]) __attribute__((weak));
extern "C" const uint8_t erhai_gpu_sqe_image[] __attribute__((weak));
extern "C" const uint64_t erhai_gpu_sqe_image_size __attribute__((weak));
extern "C" const uint8_t erhai_gpu_zap_image[] __attribute__((weak));
extern "C" const uint64_t erhai_gpu_zap_image_size __attribute__((weak));
// arm-smmu-qcom gives the GPU context bank 0; the live DT's gpu iommus are SIDs 0 and 1. ASID 2 and SMR slots 0/1 are this
// probe's choice (the GMU has bank 1, ASID 1, slot 2).
constexpr unsigned kGpuBank = 0, kGpuAsid = 2, kGpuSlot[2] = {0, 1}, kGpuSid[2] = {0, 1};
constexpr uint64_t kZapPhys = 0x9b09a000, kZapBytes = 0x2000;   // live DT gpu_microcode_mem (the zap-shader memory-region)
constexpr uint32_t kGpuPasId = 13;                               // a8xx_gpu.c GPU_PAS_ID
NativeAdrenoGmuContextReport gGpuContext;
IOReturn gGpuContextStatus = kIOReturnNotReady;
OSSharedPtr<IOMemoryMap> gGpuMaps[5], gZapMap;
volatile uint8_t *gGpuCpu[5];
NativeAdrenoGpuBuffer gGpuBuffers[5];
NativeAdrenoRing gRing;
NativeAdrenoCpInit gCpInit;
NativeAdrenoMemory gZapMetadata;
ErhaiGpuHw gHw;
ErhaiZapResult gZap;
// step, S2CR before slot 0/1, SQE mismatch offset, hw status, CP init IOReturn, CP init fence, zap result, secure-mode fence,
// post status, pwrup bytes, mem-write fence, scratch word
uint32_t gCp[13];
int64_t gZapRet = 1, gApertureRet = 1;
uint32_t gApertureAvail = 0xdead, gScmPasResumes;
uint32_t gSnap[6];
// GPU register dwords from the GPU base: the GPU window below 0x6c000, the GMU window (GPU + 0x6c000) from there.
volatile uint32_t *gpuReg(uint32_t reg) {
    const uint64_t byte = uint64_t(reg) * 4;
    if (byte >= 0x6c000) return byte - 0x6c000 < kGmuLen[0] && gGmuVa[0] ? gGmuVa[0] + (byte - 0x6c000) / 4 : nullptr;
    return gColdGpuMap && byte < kColdGpuLen ? reinterpret_cast<volatile uint32_t *>(gColdGpuMap->getVirtualAddress() + byte) : nullptr;
}
int hwWrite(void *, uint32_t reg, uint32_t value) {
    volatile uint32_t *r = gpuReg(reg);
    if (!r) return 1;
    *r = value;
    return 0;
}
int hwRead(void *, uint32_t reg, uint32_t *value) {
    volatile uint32_t *r = gpuReg(reg);
    if (!r) return 1;
    *value = *r;
    return 0;
}
int hwWait(void *, unsigned us) {IODelay(us); return 0;}
int hwBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
// adreno_hw_init: qcom_scm_set_gpu_smmu_aperture(0) when __qcom_scm_is_call_available(MP, CP_SMMU_APERTURE_ID) says so.
int hwAperture(void *) {
    uint64_t avail = 0, query = (2u << 24) | (0x0cu << 8) | 0x1bu;   // SCM_SMC_FNID(MP, 0x1b) | OWNER_SIP << 24
    uint32_t resumes = 0;
    const int64_t a0 = scmCall(0x42000000u | (0x06u << 8) | 0x01u, 1, &query, 1, resumes, &avail);
    gApertureAvail = a0 ? 0xe0000000u | uint32_t(a0 & 0xfff) : uint32_t(avail);
    if (a0 || !avail) return 1;
    const uint64_t args[4] = {0xffff0000u | 0u, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    gApertureRet = scmErrno(scmCall(0x42000000u | (0x0cu << 8) | 0x1bu, 4, args, 4, resumes));
    return int(gApertureRet);
}
const ErhaiGpuIo kHwIo = {nullptr, hwWrite, hwRead, hwWait, hwAperture};
const NativeAdrenoRingIo kRingIo = {nullptr, hwWrite, hwRead, hwBarrier};
int ringPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int ringAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
const NativeAdrenoRingCache kRingCache = {nullptr, ringPublish, ringAcquire};
uint64_t ringNow(void *) {uint64_t us = 0; coldNow(nullptr, &us); return us;}
int ringIdle(void *) {IODelay(50); return 0;}
const NativeAdrenoRingClock kRingClock = {nullptr, ringNow, ringIdle};
// The zap load's platform: the PIL SMCs with qcom_scm's result convention, and cache publication of the metadata (cacheable
// NativeAdreno memory, cleaned to PoC as dma_alloc_coherent memory would need none) or of the write-combined region.
int64_t zapPas(void *, uint32_t cmd, uint32_t arginfo, const uint64_t *args, unsigned count) {
    uint64_t result = 0;
    const int64_t a0 = scmCall(0x42000000u | (0x02u << 8) | cmd, arginfo, args, count, gScmPasResumes, &result);
    return a0 ? scmErrno(a0) : int64_t(int32_t(result));
}
int zapPublish(void *, const volatile void *ptr, size_t bytes) {
    // publishForDevice takes whole pages (the metadata is 0xfcc bytes of a 0x2000-byte object).
    if (ptr == gZapMetadata.mapping()) return gZapMetadata.publishForDevice(0, (bytes + 4095) & ~size_t(4095), false) ? 1 : 0;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
void gpuContextProgress(uint32_t step) {
    char row[48];
    snprintf(row, sizeof row, "Q step %u running", step);
    setRow(5, row);
    erhai_gpu_power_stage = 0x90 + step;
}
void hwRow() {
    // H c<step> p<hw status>/<hw step> f<failed reg> a<aperture avail>/<ret> i<CP init>/<fence> z<zap> s<secure fence>
    //   P<post status>/<pwrup bytes> m<mem-write fence>/<scratch ok>
    char row[64];
    const uint32_t *c = gCp;
    snprintf(row, sizeof row, "H c%u p%u/%u f%x a%x/%d i%x/%u z%d s%u P%u/%u m%u/%u", c[0], c[3], gHw.step, gHw.failed_reg,
             gApertureAvail, int(gApertureRet), c[4], c[5], int(gZapRet), c[7], c[8], c[9], c[10], c[11] == 0x5e9a830u);
    setRow(6, row);
    ErhaiGpuHwSnapshot(&kHwIo, gSnap);
    const volatile uint32_t *mem = gGpuCpu[2] ? reinterpret_cast<const volatile uint32_t *>(gGpuCpu[2]) : nullptr;
    snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1],
             "S4 R%x I%x C%x F%x r%x/%x m%x/%x/%x gp%x p2s%u/%x s2cr%x/%x sqe%x pwr%u/%u/%u", gSnap[0], gSnap[1], gSnap[2],
             gSnap[3], gSnap[4], gSnap[5], mem ? mem[0] : 0, mem ? mem[1] : 0, mem ? mem[2] : 0, gHw.gmem_protect,
             gHw.p2s_polls, gHw.p2s_status, c[1] & 0xffff, c[1] >> 16, c[2], gHw.pwrup_ifpc, gHw.pwrup_preemption, gHw.pwrup_dynamic);
}
void zapRow() {
    // Z<ret> s<step> i<init_image> m<mem_setup> a<auth_and_reset> n<mem_size> r<resumes> M<metadata bytes> g<segments>
    char row[64];
    snprintf(row, sizeof row, "Z%d s%u i%d m%d a%d n%llx r%u M%u g%u", int(gZapRet), gZap.step, int(gZap.init_image),
             int(gZap.mem_setup), int(gZap.auth_and_reset), static_cast<unsigned long long>(gZap.mem_size), gScmPasResumes,
             gZap.metadata_size, gZap.segments);
    setRow(7, row);
}
bool cpSubmit(const uint32_t *words, size_t count, uint32_t *result) {
    uint32_t fence = 0, seen = 0;
    if (gRing.submit(words, count, &fence)) {*result = 100; return false;}
    *result = gRing.wait(fence, &kRingClock, 200000, &seen);
    return !*result;
}
void gpuStart() {
    char row[64];
    uint32_t *c = gCp;
    if (!NativeAdrenoGpuContextBringup || !NativeAdrenoGpuContextBuffers || !&erhai_gpu_sqe_image_size || !&erhai_gpu_zap_image_size) {
        setRow(5, "S4 nolink");
        return;
    }
    // 1. Slots 0/1 must be unused; their S2CRs go to FAULT first (firmware leaves them translating to a missing bank).
    volatile uint32_t *smmu = reinterpret_cast<volatile uint32_t *>(gVa[EGB_WIN_SMMU]);
    c[0] = 1;
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned slot = kGpuSlot[i];
        c[1] |= (smmu[(0xc00 + 4 * slot) / 4] & 0xffff) << (16 * i);
        if (smmu[(0x800 + 4 * slot) / 4] & 0x80000000u) {setRow(5, "S4 skip: GPU SMR slot valid"); return;}
        smmu[(0xc00 + 4 * slot) / 4] = 0x20000u;
        __asm__ volatile("dsb sy" ::: "memory");
        if (smmu[(0xc00 + 4 * slot) / 4] != 0x20000u) {setRow(5, "S4 skip: s2cr fault"); return;}
    }
    erhai_gpu_power_stage = 0x90;
    gGpuContextStatus = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(gVa[EGB_WIN_SMMU]), EGB_SMMU_SIZE,
        kGpuBank, kGpuAsid, kGpuSlot[0], kGpuSid[0], kGpuSlot[1], kGpuSid[1], gpuContextProgress, &gGpuContext);
    const NativeAdrenoGmuContextReport &q = gGpuContext;
    // Q s<step> st<IOReturn> k<program>/<attach kit status> p<phase>e<enabled> r<route phase>v<verified> n<spans>
    snprintf(row, sizeof row, "Q s%u st%x k%u/%u p%ue%u r%uv%u n%u", q.step, unsigned(gGpuContextStatus), q.program_status,
             q.route_status, q.program_phase, q.program_enabled, q.route_phase, q.routes_verified, q.spans);
    setRow(5, row);
    if (gGpuContextStatus) return;
    // 2. The five kernel buffers through write-combine CPU views (the context maps them Normal non-cacheable).
    c[0] = 2;
    if (NativeAdrenoGpuContextBuffers(gGpuBuffers)) {hwRow(); return;}
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gGpuBuffers[i].physical, gGpuBuffers[i].bytes);
        if (m) {
            gGpuMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gGpuMaps[i]) {hwRow(); return;}
        gGpuCpu[i] = reinterpret_cast<volatile uint8_t *>(gGpuMaps[i]->getVirtualAddress());
    }
    // 3. adreno_fw_create_bo: the SQE image without its first dword, read back.
    c[0] = 3;
    const uint64_t sqe = erhai_gpu_sqe_image_size;
    if (sqe < 8 || sqe - 4 > gGpuBuffers[0].bytes) {c[2] = 0xffffffff; hwRow(); return;}
    for (uint64_t i = 4; i < sqe; ++i) gGpuCpu[0][i - 4] = erhai_gpu_sqe_image[i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint64_t i = 4; i < sqe && !c[2]; ++i) if (gGpuCpu[0][i - 4] != erhai_gpu_sqe_image[i]) c[2] = uint32_t(i);
    if (c[2]) {hwRow(); return;}
    // 4. hw_init up to CP_SQE_INSTR_BASE; the ring (CP_RB_BASE/CNTL, the RPTR shadow at memptrs +0); RPTR_ADDR_BV
    // (rbmemptr(bv_rptr), memptrs +8) and CP_SQE_CNTL; CP_THREAD_CONTROL + CP_ME_INIT and its fence.
    c[0] = 4;
    erhai_gpu_power_stage = 0x94;
    setRow(6, "H hw_init pre running");
    const ErhaiGpuHwConfig config = {gGpuBuffers[0].iova, gGpuBuffers[2].iova + 8};
    c[3] = ErhaiGpuHwInitPre(&gHw, &kHwIo, &config);
    if (c[3]) {hwRow(); return;}
    c[0] = 5;
    if (gRing.attach(&kRingIo, &kRingCache, const_cast<uint8_t *>(gGpuCpu[1]), gGpuBuffers[1].bytes, gGpuBuffers[1].iova,
                     const_cast<uint8_t *>(gGpuCpu[2]), gGpuBuffers[2].bytes, gGpuBuffers[2].iova)) {hwRow(); return;}
    c[3] = ErhaiGpuHwInitStartCp(&gHw, &kHwIo, &config);
    if (c[3]) {hwRow(); return;}
    c[0] = 6;
    erhai_gpu_power_stage = 0x96;
    setRow(6, "H cp init running");
    const NativeAdrenoCpInitConfig cp = {gGpuBuffers[0].iova, gGpuBuffers[3].iova, 1};
    uint32_t fence = 0, seen = 0;
    c[4] = uint32_t(gCpInit.run(&kRingIo, &gRing, &cp, &fence));
    if (c[4]) {hwRow(); return;}
    c[5] = gRing.wait(fence, &kRingClock, 200000, &seen);
    if (c[5]) {hwRow(); return;}
    // 5. a6xx_zap_shader_init, then CP_SET_SECURE_MODE 0 through the ring. A refused zap ends hw_init (Linux returns its
    // error there, before the OOB clear).
    c[0] = 7;
    erhai_gpu_power_stage = 0x97;
    setRow(7, "Z zap running");
    {
        IODeviceMemory *m = IODeviceMemory::withRange(kZapPhys, kZapBytes);
        if (m) {gZapMap = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
    }
    if (!gZapMap || gZapMetadata.allocate(0x2000, 40, true, 0x2000) || gZapMetadata.spanCount() != 1) {gZapRet = -12; zapRow(); hwRow(); return;}
    const ErhaiZapIo zapIo = {nullptr, zapPas, zapPublish};
    const ErhaiZapMemory memory = {static_cast<volatile uint8_t *>(gZapMetadata.mapping()), gZapMetadata.spans()[0].physical, 0x2000,
                                   reinterpret_cast<volatile uint8_t *>(gZapMap->getVirtualAddress()), kZapPhys, kZapBytes};
    gZapRet = ErhaiZapLoad(erhai_gpu_zap_image, size_t(erhai_gpu_zap_image_size), kGpuPasId, &zapIo, &memory, &gZap);
    zapRow();
    if (gZapRet) {hwRow(); return;}
    uint32_t secure[2];
    if (ErhaiGpuSecureModePacket(secure) || !cpSubmit(secure, 2, &c[7])) {if (!c[7]) c[7] = 101; hwRow(); return;}
    // 6. RB_GC_GMEM_PROTECT, the power-up list, hardware clock gating, OOB GPU_SET cleared.
    c[0] = 8;
    erhai_gpu_power_stage = 0x98;
    size_t written = 0;
    c[8] = ErhaiGpuHwInitPost(&gHw, &kHwIo, reinterpret_cast<volatile uint32_t *>(gGpuCpu[3]), gGpuBuffers[3].bytes, &written);
    c[9] = uint32_t(written);
    __asm__ volatile("dsb sy" ::: "memory");
    if (c[8]) {hwRow(); return;}
    // 7. The first command after hw_init: CP_MEM_WRITE (0x3d: address lo/hi, data) into the scratch page, then its fence.
    c[0] = 9;
    erhai_gpu_power_stage = 0x99;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gGpuCpu[4]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t words[4] = {};
    if (AdrenoPM4Type7(0x3d, 3, &words[0]) != AdrenoPM4Success) {hwRow(); return;}
    words[1] = uint32_t(gGpuBuffers[4].iova); words[2] = uint32_t(gGpuBuffers[4].iova >> 32); words[3] = 0x5e9a830u;
    cpSubmit(words, 4, &c[10]);
    c[11] = scratch[0];
    // C10: the GPU wrote the scratch word through its own context after a complete hw_init with the zap shader.
    c[0] = !c[10] && c[11] == 0x5e9a830u ? 10 : 9;
    erhai_gpu_power_stage = c[0] == 10 ? 0x9a : 0x9f;
    hwRow();
}
// S5, mode 10: after a complete S4, the GPU interrupt line, the always-on counter rate and two rounds through the user-client
// dispatch (research/a830-gpu-plan.md S5). The interrupt decode is NativeAdrenoIrq and the framed submission NativeAdrenoRing,
// both gen8 and host-compared with a8xx_irq / a7xx_submit by check-erhai-gpu-submit.py.
extern "C" {
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                           uint64_t *router) __attribute__((weak));
}
// SPI 300 of the live DT's gpu interrupts (IRQ_TYPE_LEVEL_HIGH) = INTID 332.
constexpr uint32_t kGpuIntid = 32 + 300, kIntMask = 0x62;
NativeAdrenoIrq gIrq;
NativeAdrenoSubmit gSubmit;
NativeAdrenoUserClient gClient;
volatile uint32_t gIrqCount, gIrqSeen, gIrqLast, gIrqEmpty, gIrqFatal;
// attach status, GICD readback, INT_0_MASK, irqs after attach / after one more fence, fence ok, CP and GMU counter kHz,
// user-client steps ok / total, first error / at, round 1 / 2 dwords, irqs during the rounds, close, CB0 FSR, GFSR
uint32_t gS5[20];
void gpuIrq(void *) {
    // a8xx_irq: RBBM_INT_0_STATUS read and cleared through RBBM_INT_CLEAR_CMD first, so the level line drops before the EOI.
    // A line with nothing to clear (a storm) masks the GPU interrupts instead of hanging the CPU it is routed to.
    NativeAdrenoIrqResult r;
    if (gIrq.handle(&r)) return;
    gIrqLast = r.status;
    gIrqSeen = gIrqSeen | r.status;
    gIrqCount = gIrqCount + 1;
    if (r.flags & NativeAdrenoIrqFatal) gIrqFatal = gIrqFatal + 1;
    gIrqEmpty = r.status ? 0 : gIrqEmpty + 1;
    if (gIrqEmpty > 100000) hwWrite(nullptr, kIntMask, 0);
}
// The always-on counters: CP_ALWAYS_ON_COUNTER (GPU dwords) and GMU_ALWAYS_ON_COUNTER (GMU-window bytes), as NativeAdrenoTimer reads them.
int timerCpRead(void *, uint32_t reg, uint32_t *value) {return hwRead(nullptr, reg, value);}
int timerGmuRead(void *, uint32_t offset, uint32_t *value) {
    if (!gGmuVa[0] || offset >= kGmuLen[0] || (offset & 3)) return 1;
    *value = gGmuVa[0][offset / 4];
    return 0;
}
uint64_t cntvct() {uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v)); return v;}
// kHz of a counter against CNTVCT (19.2 MHz) over 100 ms; 0 when a read fails.
uint32_t counterKhz(const NativeAdrenoTimer &t) {
    uint64_t g0 = 0, g1 = 0;
    const uint64_t t0 = cntvct();
    if (t.read(&g0)) return 0;
    IOSleep(100);
    const uint64_t t1 = cntvct();
    if (t.read(&g1) || t1 <= t0) return 0;
    return uint32_t((g1 - g0) * 19200 / (t1 - t0));
}
uint32_t gpuFsr(uint32_t offset) {
    const uint32_t id1 = gGpuContext.id1, cb = egb_smmu_cb_base(id1) + kGpuBank * egb_smmu_page(id1);
    if (!gVa[EGB_WIN_SMMU] || !id1 || cb + offset + 4 > EGB_SMMU_SIZE) return 0xdead;
    return *reinterpret_cast<volatile uint32_t *>(gVa[EGB_WIN_SMMU] + cb + offset);
}
IOReturn ucCall(uint32_t method, const void *in, void *out) {
    char mark[48];
    snprintf(mark, sizeof mark, "U run m%u", method);
    setRow(7, mark);
    const size_t os = NativeAdrenoUserClient::outputSize(method);
    return gClient.dispatch(method, in, NativeAdrenoUserClient::inputSize(method), os ? out : nullptr, os, &kRingClock);
}
// One round through the user-client dispatch, as sheng's ucRound: an IB and a target object mapped at the given GPU addresses,
// the IB's three CP_MEM_WRITEs into the target, submitted (framed), waited on, read back by the CPU (three written dwords and
// one untouched), cpu-mapped and unmapped, then unmapped and freed (live SMMU unmap and TLB invalidate). Returns the matching
// dwords of 4 or -1.
int ucRound(unsigned round, uint64_t ibIova, uint64_t tgtIova, uint32_t pattern, IOReturn &err, int &errAt, int &ok, int &n) {
    auto step = [&](IOReturn s) {
        const int idx = n++;
        if (!s) ++ok;
        else if (!err) {err = s; errAt = idx;}
        return !s;
    };
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    if (!step(ucCall(NativeAdrenoMethodBoAlloc, &alloc, &ib)) || !step(ucCall(NativeAdrenoMethodBoAlloc, &alloc, &tgt))) return -1;
    NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply r1{}, r2{};
    if (!step(ucCall(NativeAdrenoMethodBoMap, &mapIb, &r1)) || !step(ucCall(NativeAdrenoMethodBoMap, &mapTgt, &r2))) return -1;
    NativeAdrenoMemory *ibMem = gClient.memoryOf(ib.handle), *tgtMem = gClient.memoryOf(tgt.handle);
    if (!ibMem || !tgtMem) {step(kIOReturnNotFound); return -1;}
    volatile uint32_t *iw = static_cast<volatile uint32_t *>(ibMem->mapping()), *tw = static_cast<volatile uint32_t *>(tgtMem->mapping());
    for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
    const uint32_t values[3] = {pattern, ~pattern, 0x600d0000u | round};
    uint32_t words[12];
    for (unsigned i = 0; i < 3; ++i) {
        if (AdrenoPM4Type7(0x3d, 3, &words[i * 4]) != AdrenoPM4Success) {step(kIOReturnInternalError); return -1;}
        words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32); words[i * 4 + 3] = values[i];
    }
    for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
    if (!step(ibMem->publishForDevice(0, 4096, false)) || !step(tgtMem->publishForDevice(0, 4096, true))) return -1;
    NativeAdrenoAbiSubmit submit{};
    submit.buffer_count = 1;
    submit.buffers[0] = {ib.handle, ibIova, sizeof words};
    NativeAdrenoAbiSubmitReply sr{};
    int matched = -1;
    if (step(ucCall(NativeAdrenoMethodSubmit, &submit, &sr))) {
        NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
        if (step(ucCall(NativeAdrenoMethodWaitFence, &wait, nullptr)) && step(tgtMem->acquireDeviceWrites(0, 4096)))
            matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
    }
    NativeAdrenoAbiBoCpuMap cm{tgt.handle};
    NativeAdrenoAbiBoCpuMapReply cr{};
    if (step(ucCall(NativeAdrenoMethodBoCpuMap, &cm, &cr))) {
        NativeAdrenoAbiBoCpuUnmap cu{tgt.handle, cr.token, 0};
        step(ucCall(NativeAdrenoMethodBoCpuUnmap, &cu, nullptr));
    }
    NativeAdrenoAbiBoFree f1{ib.handle}, f2{tgt.handle};
    step(ucCall(NativeAdrenoMethodBoFree, &f1, nullptr));
    step(ucCall(NativeAdrenoMethodBoFree, &f2, nullptr));
    return matched;
}
void s5Rows() {
    // Row 5: I a<attach> b<GICD readback> k<INT_0_MASK> n<irqs after attach>/<after a fence><fence ok> l<status seen>
    //   T<CP counter kHz>/<GMU counter kHz>
    // Row 7: U ok<steps ok>/<steps> e<first error>@<step> v<round 1>/<round 2 dwords of 4> q<irqs> c<close> s<CB0 FSR> G<GFSR>
    char row[64];
    const uint32_t *u = gS5;
    snprintf(row, sizeof row, "I a%x b%x k%x n%u/%u%c l%x T%u/%u", u[0], u[1], u[2], u[3], u[4], u[5] ? '+' : '-', gIrqSeen,
             u[6], u[7]);
    setRow(5, row);
    snprintf(row, sizeof row, "U ok%u/%u e%x@%d v%d/%d q%u c%x s%x G%x", u[8], u[9], u[10], int(u[11]), int(u[12]), int(u[13]),
             u[14], u[15], u[16], u[17]);
    setRow(7, row);
}
void gpuSubmitTest() {
    uint32_t *u = gS5;
    u[11] = uint32_t(-1); u[12] = u[13] = uint32_t(-1);
    erhai_gpu_power_stage = 0xa0;
    setRow(5, "I irq attach running");
    // 1. The interrupt line. The S4 fences left CP_CACHE_FLUSH_TS set, so a working line fires as soon as it is enabled; one
    // more fence must fire it again.
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    if (!gIrq.attach(&kRingIo)) {
        u[0] = armcpu_spi_attach ? uint32_t(armcpu_spi_attach(kGpuIntid, false, gpuIrq, nullptr, &readback, &router)) : 0xdead;
    } else {
        u[0] = 0xbad;
    }
    u[1] = readback;
    hwRead(nullptr, kIntMask, &u[2]);
    IOSleep(20);
    u[3] = gIrqCount;
    uint32_t nop = 0, result = 0;
    u[5] = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success && cpSubmit(&nop, 1, &result);
    IOSleep(20);
    u[4] = gIrqCount;
    s5Rows();
    // 2. The always-on counters against CNTVCT.
    erhai_gpu_power_stage = 0xa1;
    NativeAdrenoTimer cp, gmu;
    const NativeAdrenoTimeIo cpIo{nullptr, timerCpRead}, gmuIo{nullptr, timerGmuRead};
    if (!cp.attach(&cpIo, NativeAdrenoTimeCp)) u[6] = counterKhz(cp);
    if (!gmu.attach(&gmuIo, NativeAdrenoTimeGmu)) u[7] = counterKhz(gmu);
    s5Rows();
    // 3. Two user-client rounds. Round 2 reuses the IB address with a new object (a stale TLB entry would run the old IB, which
    // writes to the now unmapped round-1 target and faults) and maps its target at a fresh address.
    erhai_gpu_power_stage = 0xa2;
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    IOReturn err = 0;
    int errAt = -1, ok = 0, n = 0;
    IOReturn opened = !vm ? kIOReturnNotReady : gSubmit.attach(vm, &gRing);
    // input_bits 31: the 2 GiB TTBR0 aperture; output_bits from the SMMU's ID2.OAS
    if (!opened) opened = gClient.open(NativeAdrenoAbiVersion, vm, &gSubmit, &gRing, 31, gGpuContext.output_bits);
    if (opened) {err = opened; errAt = 0; u[10] = uint32_t(err); u[11] = 0; s5Rows(); return;}
    const uint32_t irq0 = gIrqCount;
    erhai_gpu_power_stage = 0xa3;
    const int v1 = ucRound(1, 0x10000000, 0x10001000, 0x5e9a8300u, err, errAt, ok, n);
    erhai_gpu_power_stage = 0xa4;
    const int v2 = ucRound(2, 0x10000000, 0x10002000, 0xa830b0b0u, err, errAt, ok, n);
    u[8] = uint32_t(ok); u[9] = uint32_t(n); u[10] = uint32_t(err); u[11] = uint32_t(errAt);
    u[12] = uint32_t(v1); u[13] = uint32_t(v2);
    u[14] = gIrqCount - irq0;
    u[15] = uint32_t(gClient.close());
    u[16] = gpuFsr(0x58);
    u[17] = gVa[EGB_WIN_SMMU] ? *reinterpret_cast<volatile uint32_t *>(gVa[EGB_WIN_SMMU] + EGB_SMMU_GFSR) : 0xdead;
    s5Rows();
    // arm-smmu's ARM_SMMU_CB_FSR_FAULT bits (MULTI, SS, UUT, ASF, TLBLKF, TLBMCF, EF, PF, AFF, TF); FORMAT (bits 9..10) is no fault.
    erhai_gpu_power_stage = v1 == 4 && v2 == 4 && !(u[16] & 0xc00001feu) && u[4] > u[3] ? 0xaa : 0xaf;
}
void gmuPrerequisites(const egb_result &r) {
    char row[64];
    if (gContextStatus) {setRow(6, "S2 skip: GMU context not ready"); return;}
    // Row 5 keeps the S1 result in short form; rows 6/7 and the wide rows now carry S2.
    const NativeAdrenoGmuContextReport &g = gContext;
    snprintf(row, sizeof row, "S1 s%u st%x k%u/%u r%uv%u walk%u/%u", g.step, unsigned(gContextStatus), g.program_status,
             g.route_status, g.route_phase, g.routes_verified, gWalkOk, gWalked);
    setRow(5, row);
    for (unsigned w = 0; w < 3; ++w) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kGmuPhys[w], kGmuLen[w]);
        if (!memory) continue;
        gGmuMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
        if (gGmuMaps[w]) gGmuVa[w] = reinterpret_cast<volatile uint32_t *>(gGmuMaps[w]->getVirtualAddress());
    }
    erhai_gpu_power_stage = 0x50;
    readClocks(gClocks);
    if (gMode >= 6) {
        erhai_gpu_power_stage = 0x60;
        gmuClocksOn();
        readClocks(gClocks);
    }
    const GmuClocks &k = gClocks;
    snprintf(row, sizeof row, "CK p%x/%x l%x u%x g%x/%x h%x/%x/%x", k.pll_mode, k.pll_opmode, k.pll_l, k.pll_user, k.gmu_cmd,
             k.gmu_cfg, k.hub_cmd, k.hub_cfg, k.hub_div);
    setRow(6, row);
    // Mode 6 shows its result instead of the branch words (already in mode 5's photo and still in the dump).
    if (gMode >= 6)
        snprintf(row, sizeof row, "C6 s%u f%u lk%u v%x>%x p%x/%x>%x", gClockStep, gClockFail, gLockUs, gVoteBefore, gVoteAfter,
                 gClocksBefore.pll_mode, gClocksBefore.gmu_cfg, gClocksBefore.hub_cfg);
    else
        snprintf(row, sizeof row, "BR c%x x%x i%x a%x h%x m%x v%x", k.cx_gmu, k.cxo, k.hub_cx_int, k.hub_aon, k.ahb, k.memnoc_gfx,
                 r.pll_vote);
    setRow(7, row);
    erhai_gpu_power_stage = 0x51;
    buildVotes(r);
    const AdrenoHfiStartupConfig &c = gStartup;
    const unsigned top = c.gpu_count ? c.gpu_count - 1 : 0;
    snprintf(boot_progress_ext_wide[0], sizeof boot_progress_ext_wide[0],
             "DB v%x s%u p%u bin%u gx%u gm%u bw%u c%u/%u top%llu %x/%x gmu%x/%x x%u acv%x", gDbVersion, gDbStatus, gVoteStatus,
             gVoteInputs.speed_bin_fuse, c.gpu_count, gVoteInputs.gmxc_present, c.bandwidth_count, c.cnoc_count, c.ddr_count,
             static_cast<unsigned long long>(c.gpu[top].hz / 1000000), c.gpu[top].vote, c.gpu_dependency[top],
             c.gmu[1].vote, c.gmu[2].vote, c.table_format, gVoteInputs.acv.address);
    erhai_gpu_power_stage = 0x52;
    // GMU registers only with its clock running (gpu_cc_cx_gmu_clk CLK_OFF clear), as sheng's mode 5.
    if (k.cx_gmu & EGB_CBCR_CLK_OFF) {
        snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1], "GMU not read: cx_gmu clock off (%08x)", k.cx_gmu);
    } else if (readGmuState(gGmuState)) {
        const GmuState &s = gGmuState;
        snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1],
                 "GMU rst%x init%x hfi%x ver%x pwr%x rpmh%x ao%x info%x tcm%x busy%x", s.reset, s.init, s.hfi, s.version, s.pwr,
                 s.rpmh, s.ao, s.info, s.tcm, s.busy);
    }
    erhai_gpu_power_stage = 0x5f;
    if (gMode < 7) return;
    const GmuState &s = gGmuState;
    const bool ready = gClockStep == kClkDone && !gClockFail && gDbStatus == ShengGmuDbSuccess &&
                       gVoteStatus == AdrenoHfiStartupSuccess && !(k.cx_gmu & EGB_CBCR_CLK_OFF) && s.reset == 1 && !s.init &&
                       !s.hfi && !s.tcm && !s.busy;
    if (!ready) {setRow(5, "S2 m7 skip: prerequisites"); return;}
    gmuColdStart();
    readGmuState(gGmuState);
    // Row 5: c<cold step> si<SCM a0>.<resumes> f<SW_FUSE before>><after>; row 6: the HFI protocol result (P status, g stage,
    // k acked stage mask, e firmware error, t transport, n published, elapsed, s/h/r skipped replies); row 7: the load (S
    // ShengGmuStatus, ph phase, h hash, f FW_INIT_RESULT, H HFI status, r FW/HFI ready, a AO fault, c CM3 fault, v reset
    // verified); wide row 1: the GMU state after.
    snprintf(row, sizeof row, "M7 c%u si%lld.%u f%x>%x S%u", gColdStep, static_cast<long long>(gScmRet), gScmResumes,
             gFuseBefore, gFuseAfter, gColdStatus);
    setRow(5, row);
    const AdrenoHfiStartupResult &p = gColdResult.protocol;
    const AdrenoHfi &q = gColdDevice.hfi;
    snprintf(row, sizeof row, "P%u g%u k%x e%x t%u n%u %llums s%u h%x r%x", gColdResult.protocol_status, p.stage,
             p.acknowledged_stages, p.firmware_error, p.transport_status, p.published_commands,
             static_cast<unsigned long long>(p.elapsed_us / 1000), q.skipped, q.skipped_header, q.skipped_ret);
    setRow(6, row);
    const ShengGmuResult &l = gColdResult.load;
    snprintf(row, sizeof row, "S%u ph%u h%u f%x H%x r%u%u a%x c%x v%u", gColdStatus, l.phase, l.complete_hash_verified,
             l.firmware_init_result, l.hfi_status, l.firmware_ready_observed, l.hfi_ready_observed, l.ao_fault_status,
             l.cm3_fault_status, l.reset_verified);
    setRow(7, row);
    snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1],
             "GMU after rst%x init%x hfi%x ver%x pwr%x rpmh%x ao%x info%x tcm%x busy%x", s.reset, s.init, s.hfi, s.version, s.pwr,
             s.rpmh, s.ao, s.info, s.tcm, s.busy);
    if (gMode < 8 || gColdStatus != ShengGmuSuccess) return;
    gxOn();
    // Row 4: X v<vote status>/<fw error> o<ack polls> x<PWR_CLK_STATUS before>><after> P<perf>/<bw> R<RBBM_STATUS>
    //   H<GBIF_HALT>/<RBBM_GBIF_HALT>; wide row 0 gets the gxclkctl GX GDSCR before > after.
    const uint32_t *x = gGx;
    snprintf(row, sizeof row, "X v%u/%x o%u x%x>%x P%u/%u R%x H%x/%x", x[0], x[1], x[2], x[3], x[4], gPerf, gBw, x[7], x[8], x[9]);
    setRow(4, row);
    const size_t used = strnlen(boot_progress_ext_wide[0], sizeof boot_progress_ext_wide[0]);
    snprintf(boot_progress_ext_wide[0] + used, sizeof boot_progress_ext_wide[0] - used, " gx%x>%x", x[5], x[6]);
    if (gMode < 9 || x[2] > 100 || (x[4] & 3)) return;
    gpuStart();
    if (gMode >= 10 && gCp[0] == 10) gpuSubmitTest();
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
// A CPU stuck in the probe (an SMC that does not return, a stalled bus access) also holds the timers queued on it: in
// mode 7 the watchdog's IOSleep was one of them and nothing ever panicked. With every registered CPU online, the probe runs
// bound to the last CPU and the watchdog to CPU 0, so the watchdog's timer is never on the probe's CPU.
bool gBind;
int gProbeCpu = -1;
void bindTo(int cpu) {
    if (!gBind) return;
    thread_bind(cpu_to_processor(cpu));
    thread_block(THREAD_CONTINUE_NULL);
}
void probeThread(void *, wait_result_t) {
    bindTo(int(processor_count) - 1);
    gProbeCpu = cpu_number();
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
    if (gMode >= 5) gmuPrerequisites(erhai_gpu_power_result);
    IOLog("ERHAI_GPU %.47s | %.47s | %.47s | %.47s\nERHAI_GPU %.111s\nERHAI_GPU %.111s\n", boot_progress_ext[4],
          boot_progress_ext[5], boot_progress_ext[6], boot_progress_ext[7], boot_progress_ext_wide[0], boot_progress_ext_wide[1]);
    unsigned dumpArg = 0;
    gFinished = true;
    if (PE_parse_boot_argn("erhai-gpu-dump", &dumpArg, sizeof dumpArg) && dumpArg) dump(erhai_gpu_power_result);
}
// With erhai-gpu-dump, a probe that stops without faulting (a stalled access or a wait that never ends) would otherwise leave
// nothing visible once iOS owns the display: after a minute, panic with the stage and the rows written so far.
void watchdogThread(void *, wait_result_t) {
    bindTo(0);
    for (unsigned s = 0; s < 70 && !gFinished; ++s) IOSleep(1000);
    if (gFinished) return;
    const ShengGmuResult &l = gColdResult.load;
    panic("ERHAI_GPU WATCHDOG m%u stage=%x cpu%d/%d bind%u avail%u/%u scm%lld.%u cold%u ph%u w%u r%u p%u cp%u hw%u z%u\n"
          "%.47s\n%.47s\n%.47s\n%.47s\n%.111s\n%.111s\n", gMode, erhai_gpu_power_stage, gProbeCpu, cpu_number(), gBind,
          processor_avail_count, processor_count, static_cast<long long>(gScmRet), gScmResumes, gColdStep, l.phase,
          l.write_attempts, l.read_attempts, l.poll_reads, gCp[0], gHw.step, gZap.step, boot_progress_ext[4], boot_progress_ext[5], boot_progress_ext[6],
          boot_progress_ext[7], boot_progress_ext_wide[0], boot_progress_ext_wide[1]);
}
}

extern "C" void erhai_gpu_power_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("erhai-gpu-power", &mode, sizeof mode) || !mode || erhai_gpu_power_stage) return;
    gMode = mode > 10 ? 10 : mode;
    gBind = processor_count > 1 && processor_avail_count == processor_count;
    erhai_gpu_power_stage = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else erhai_gpu_power_stage = 0xdead;
    unsigned dumpArg = 0;
    if (PE_parse_boot_argn("erhai-gpu-dump", &dumpArg, sizeof dumpArg) && dumpArg &&
        kernel_thread_start(watchdogThread, nullptr, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
