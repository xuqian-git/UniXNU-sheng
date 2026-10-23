// SPDX-License-Identifier: BSD-2-Clause-Patent
// erhai (SM8750) Adreno 830 power/clock and GPU-SMMU observation: board facts plus the staged, gated sequence (S0 of
// research/a830-gpu-plan.md). SM8550's GpuPowerBoard.h has the same shape; the offsets below are SM8750's and every one names
// its source. Nothing here touches hardware; ErhaiGpuPower.cpp owns the MMIO. Host-tested by ErhaiGpuBoardTests.c.
//
// Why staged: the GPU SMMU and GPUCC sit behind clocks and power domains Darwin has no driver for, and reading a block whose
// clock or rail is off can hang the bus instead of failing. Each stage runs only if the previous one showed its prerequisite on.
// The question this answers first: which stream-match entries and context banks of the adreno SMMU the hypervisor leaves
// visible (apps_smmu showed only SID 0x800 to NativeUfs), and whether the GPU (SIDs 0/1) and GMU (SID 5) streams are free.
#ifndef ERHAI_GPU_BOARD_H
#define ERHAI_GPU_BOARD_H
#include <stdint.h>

// Live DTB (device-dump/20261023-002031): gcc clock-controller@100000, gpucc clock-controller@3d90000 size 0x9800,
// gxclkctl 0x3d68024 (gxclkctldebugcc syscon 0x3d64000 size 0x6000), kgsl-smmu@3da0000 size 0x40000,
// qfprom@221c8000 gpu_speed_bin@138 bits 0..8, aop_cmd_db_region@81c60000 size 0x20000.
#define EGB_GCC_GPU_PAGE 0x00171000ULL   // GCC base + 0x71000
#define EGB_GCC_VOTE_PAGE 0x00152000ULL  // GCC base + 0x52000
#define EGB_GPUCC_BASE 0x03d90000ULL
#define EGB_GPUCC_SIZE 0x9800ULL
#define EGB_GXCLK_PAGE 0x03d68000ULL     // gxclkctl-kaanapali.c: base 0x3d64000, gx_clkctl_gx_gdsc gdscr 0x4024
#define EGB_SMMU_BASE 0x03da0000ULL
#define EGB_SMMU_SIZE 0x40000ULL
#define EGB_FUSE_PAGE 0x221c8000ULL
#define EGB_CMDDB_BASE 0x81c60000ULL

// gcc-sm8750.c: gcc_gpu_cfg_ahb_clk 0x71004 (enabled by the probe, qcom_branch_set_clk_en), gcc_gpu_gemnoc_gfx_clk 0x71010,
// gcc_ddrss_gpu_axi_clk 0x71150; gcc_gpu_gpll0_clk_src / _div_clk_src vote 0x52000 bits 15/16. Offsets inside their page.
#define EGB_GCC_GPU_CFG_AHB 0x004
#define EGB_GCC_GPU_GEMNOC_GFX 0x010
#define EGB_GCC_DDRSS_GPU_AXI 0x150
#define EGB_GCC_PLL_VOTE 0x000
#define EGB_CBCR_CLK_OFF (1u << 31)       // clk-branch.c BRANCH_CLK_OFF
#define EGB_CBCR_ENABLE (1u << 0)

// gpucc-sm8750.c: gpu_cc_cx_gdsc gdscr 0x9080 / gds_hw_ctrl 0x9094 (RETAIN_FF_ENABLE | VOTABLE, en_rest 2, en_few 2,
// clk_dis 8); gpu_cc_ahb_clk 0x90bc (BRANCH_HALT_DELAY); gpu_cc_hlos1_vote_gpu_smmu_clk 0x7000 (BRANCH_HALT_VOTED);
// gpu_cc_cx_gmu_clk 0x90d4; gpu_cc_cxo_clk 0x90e4; gpu_cc_hub_cx_int_clk 0x90e8; gpu_cc_gx_gmu_clk 0x9060; GX BCR 0x905c.
#define EGB_CC_CX_GDSCR 0x9080
#define EGB_CC_CX_GDS_HW_CTRL 0x9094
#define EGB_CC_AHB_CLK 0x90bc
#define EGB_CC_SMMU_VOTE_CLK 0x7000
#define EGB_CC_CX_GMU_CLK 0x90d4
#define EGB_CC_CXO_CLK 0x90e4
#define EGB_CC_HUB_CX_INT_CLK 0x90e8
#define EGB_CC_GX_GMU_CLK 0x9060
#define EGB_CC_GX_BCR 0x905c
// gxclkctl GDSC: gdscr 0x4024 of the 0x3d64000 block = 0x24 of the 0x3d68000 page; POLL_CFG_GDSCR, so status is CFG_GDSCR (+4)
// bit 16 POWER_UP_COMPLETE / bit 15 POWER_DOWN_COMPLETE. The GMU firmware owns it; it is only read.
#define EGB_GX_GDSCR 0x024
#define EGB_GX_CFG_GDSCR 0x028

// gdsc.c: PWR_ON BIT(31), SW_COLLAPSE BIT(0), HW_CONTROL BIT(1), SW_OVERRIDE BIT(2), wait fields 23:20 / 19:16 / 15:12,
// RETAIN_FF_ENABLE BIT(11), STATUS_POLL_TIMEOUT_US 2000.
#define EGB_GDSC_PWR_ON (1u << 31)
#define EGB_GDSC_SW_COLLAPSE (1u << 0)
#define EGB_GDSC_INIT_MASK ((1u << 1) | (1u << 2) | (0xfu << 20) | (0xfu << 16) | (0xfu << 12))
#define EGB_GDSC_CX_WAITS ((2u << 20) | (2u << 16) | (8u << 12))
#define EGB_GDSC_RETAIN_FF (1u << 11)
#define EGB_GDSC_POLL_US 2000
#define EGB_CFG_POWER_UP_COMPLETE (1u << 16)
#define EGB_CFG_POWER_DOWN_COMPLETE (1u << 15)

// ARM SMMUv2 (IHI 0062D.c), MMU-500: sCR0 0x0, ID0 0x20, ID1 0x24, ID2 0x28, ID7 0x3c, sGFSR 0x48, SMR(n) 0x800+4n
// (VALID bit 31, MASK 30:16, ID 14:0), S2CR(n) 0xc00+4n (TYPE 17:16: 0 translate, 1 bypass, 2 fault; CBNDX 7:0),
// GR1 CBAR(n) at page 1 + 4n. Context banks start at 2^(NUMPAGENDXB+1) pages; PAGESIZE is ID1 bit 31 (64 KiB when set).
#define EGB_SMMU_SCR0 0x0
#define EGB_SMMU_ID0 0x20
#define EGB_SMMU_ID1 0x24
#define EGB_SMMU_ID2 0x28
#define EGB_SMMU_ID7 0x3c
#define EGB_SMMU_GFSR 0x48
#define EGB_SMMU_SMR(n) (0x800u + 4u * (n))
#define EGB_SMMU_S2CR(n) (0xc00u + 4u * (n))
#define EGB_SMMU_MAX_SMR 128u
#define EGB_SMMU_MAX_SHOWN 24u
// The streams the live DTB names: gfx3d_user 0, gfx3d_lpac 1, gfx3d_secure 2, gmu 5, iommu-debug usecase5_kgsl 7.
#define EGB_SIDS 5u
static const uint16_t egb_sids[EGB_SIDS] = {0, 1, 2, 5, 7};
#define EGB_NO_MATCH 0xffu

#define EGB_FUSE_SPEED_BIN 0x138          // 16-bit cell at 0x138, bits 0..8; read as the aligned word
#define EGB_CMDDB_MAGIC 0x0c0330dbu       // cmd-db.c CMD_DB_MAGIC, header bytes 4..7

enum egb_state {EGB_NOT_RUN = 0, EGB_DONE = 1, EGB_SKIPPED_PREREQ = 2, EGB_SKIPPED_MODE = 3, EGB_NOMAP = 4};

struct egb_io {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);   // 0 on success
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);                                     // may be NULL in host tests
};
enum {EGB_WIN_GCC_GPU = 0, EGB_WIN_GCC_VOTE, EGB_WIN_GPUCC, EGB_WIN_GXCLK, EGB_WIN_SMMU, EGB_WIN_FUSE, EGB_WIN_CMDDB, EGB_WINDOWS};

struct egb_smr {uint8_t index, cb, type, pad; uint32_t smr, s2cr;};
struct egb_result {
    unsigned mode;                       // 1 read-only; 2 + enable gcc_gpu_cfg_ahb_clk; 3 + CX GDSC and the SMMU clocks
    enum egb_state gcc, cc, gx, smmu, fuse, cmddb;
    uint32_t cfg_ahb, cfg_ahb_after, gemnoc, axi, pll_vote;
    uint32_t cx_before, cx_gdscr, cx_hwctrl, cc_ahb, cc_smmu_before, cc_smmu, cc_gmu, cc_cxo, cc_hub, cc_gx_gmu, gx_bcr;
    uint32_t gx_gdscr, gx_cfg;
    unsigned enabled_ahb, powered_cx, powered_clocks;
    uint32_t scr0, id0, id1, id2, id7, gfsr;
    unsigned num_smr, num_cb, smr_valid, types[4], shown;
    struct egb_smr smr[EGB_SMMU_MAX_SHOWN];
    uint8_t sid_slot[EGB_SIDS], sid_cb[EGB_SIDS], sid_type[EGB_SIDS];   // EGB_NO_MATCH when no valid SMR matches
    uint32_t sid_cbar[EGB_SIDS], sid_sctlr[EGB_SIDS];
    uint32_t speed_bin_word, cmddb_version, cmddb_magic;
    int io_error;
};

static inline int egb_clock_on(uint32_t cbcr) {return (cbcr & EGB_CBCR_CLK_OFF) == 0;}
// gcc_gpu_cfg_ahb_clk has hardware clock gating: idle it reads CLK_OFF with the enable bit set, so the enable bit is the test.
static inline int egb_cfg_ahb_enabled(uint32_t cbcr) {return (cbcr & EGB_CBCR_ENABLE) != 0;}
static inline int egb_gdsc_on(uint32_t word) {return (word & EGB_GDSC_PWR_ON) != 0;}
static inline unsigned egb_smr_match(uint32_t smr, uint16_t sid) {
    uint32_t id = smr & 0x7fffu, mask = (smr >> 16) & 0x7fffu;
    return (smr & (1u << 31)) && ((sid & ~mask & 0x7fffu) == (id & ~mask));
}
// Page geometry from ID1: PAGESIZE bit 31, NUMPAGENDXB 30:28.
static inline uint32_t egb_smmu_page(uint32_t id1) {return (id1 & (1u << 31)) ? 0x10000u : 0x1000u;}
static inline uint32_t egb_smmu_cb_base(uint32_t id1) {return egb_smmu_page(id1) << (((id1 >> 28) & 7u) + 1u);}

static inline int egb_rd(const struct egb_io *io, struct egb_result *r, unsigned w, uint32_t off, uint32_t *out) {
    uint32_t v = 0;
    if (io->read32(io->cookie, w, off, &v)) {r->io_error = 1; *out = 0; return -1;}
    *out = v;
    return 0;
}
static inline int egb_wr(const struct egb_io *io, struct egb_result *r, unsigned w, uint32_t off, uint32_t v) {
    if (io->write32(io->cookie, w, off, v)) {r->io_error = 1; return -1;}
    return 0;
}

// Mode 3: gdsc_init's wait-value programming, then gdsc_enable for gpu_cc_cx_gdsc (collapse bit clear, 1 us, poll gds_hw_ctrl
// PWR_ON for 2 ms, 1 us, RETAIN_FF), then the two branches the adreno SMMU node lists (gpu_cc_ahb_clk by its parent's needs,
// gpu_cc_hlos1_vote_gpu_smmu_clk as "hlos"), waiting for the voted branch's CLK_OFF to clear.
static inline void egb_power_cx(const struct egb_io *io, struct egb_result *r) {
    uint32_t v = r->cx_gdscr, st = 0;
    if (egb_wr(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDSCR, (v & ~EGB_GDSC_INIT_MASK) | EGB_GDSC_CX_WAITS)) return;
    if (egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDSCR, &v) || egb_wr(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDSCR, v & ~EGB_GDSC_SW_COLLAPSE))
        return;
    if (io->delay_us) io->delay_us(io->cookie, 1);
    for (unsigned waited = 0;; waited += 10) {
        if (egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDS_HW_CTRL, &st)) return;
        if (egb_gdsc_on(st) || waited >= EGB_GDSC_POLL_US) break;
        if (io->delay_us) io->delay_us(io->cookie, 10);
    }
    r->cx_hwctrl = st;
    if (!egb_gdsc_on(st)) return;
    if (io->delay_us) io->delay_us(io->cookie, 1);
    if (egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDSCR, &v) || egb_wr(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDSCR, v | EGB_GDSC_RETAIN_FF)) return;
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDSCR, &r->cx_gdscr);
    r->powered_cx = 1;
}
static inline void egb_smmu_clocks(const struct egb_io *io, struct egb_result *r) {
    uint32_t a = 0, c = 0;
    if (egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_AHB_CLK, &a) || egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_SMMU_VOTE_CLK, &c)) return;
    if (egb_wr(io, r, EGB_WIN_GPUCC, EGB_CC_AHB_CLK, a | EGB_CBCR_ENABLE)) return;
    if (io->delay_us) io->delay_us(io->cookie, 10);                     // BRANCH_HALT_DELAY
    if (egb_wr(io, r, EGB_WIN_GPUCC, EGB_CC_SMMU_VOTE_CLK, c | EGB_CBCR_ENABLE)) return;
    for (unsigned waited = 0;; waited += 10) {
        if (egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_SMMU_VOTE_CLK, &r->cc_smmu)) return;
        if (egb_clock_on(r->cc_smmu) || waited >= 500) break;
        if (io->delay_us) io->delay_us(io->cookie, 10);
    }
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_AHB_CLK, &r->cc_ahb);
    r->powered_clocks = (unsigned)egb_clock_on(r->cc_smmu);
}

// Stream routing, read only: global IDs, every valid SMR with its S2CR (the first EGB_SMMU_MAX_SHOWN kept), and for each DT
// stream the first matching entry, its bank's CBAR and the bank's SCTLR.
static inline void egb_read_smmu(const struct egb_io *io, struct egb_result *r) {
    egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_SCR0, &r->scr0);
    egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_ID0, &r->id0);
    egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_ID1, &r->id1);
    egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_ID2, &r->id2);
    egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_ID7, &r->id7);
    egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_GFSR, &r->gfsr);
    if (r->io_error) return;
    r->num_smr = r->id0 & 0xffu;
    r->num_cb = r->id1 & 0xffu;
    if (r->num_smr > EGB_SMMU_MAX_SMR) r->num_smr = EGB_SMMU_MAX_SMR;
    const uint32_t page = egb_smmu_page(r->id1), cb_base = egb_smmu_cb_base(r->id1);
    for (unsigned s = 0; s < EGB_SIDS; ++s) r->sid_slot[s] = r->sid_cb[s] = r->sid_type[s] = EGB_NO_MATCH;
    for (unsigned i = 0; i < r->num_smr; ++i) {
        uint32_t smr = 0, s2cr = 0;
        if (egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_SMR(i), &smr)) return;
        if (!(smr & (1u << 31))) continue;
        if (egb_rd(io, r, EGB_WIN_SMMU, EGB_SMMU_S2CR(i), &s2cr)) return;
        const unsigned type = (s2cr >> 16) & 3u, cb = s2cr & 0xffu;
        ++r->smr_valid;
        ++r->types[type];
        if (r->shown < EGB_SMMU_MAX_SHOWN) r->smr[r->shown++] = (struct egb_smr){(uint8_t)i, (uint8_t)cb, (uint8_t)type, 0, smr, s2cr};
        for (unsigned s = 0; s < EGB_SIDS; ++s)
            if (r->sid_slot[s] == EGB_NO_MATCH && egb_smr_match(smr, egb_sids[s])) {
                r->sid_slot[s] = (uint8_t)i; r->sid_cb[s] = (uint8_t)cb; r->sid_type[s] = (uint8_t)type;
            }
    }
    for (unsigned s = 0; s < EGB_SIDS; ++s) {
        if (r->sid_type[s] != 0 || r->sid_cb[s] >= r->num_cb) continue;
        const uint32_t cb = r->sid_cb[s];
        if (cb_base + (cb + 1) * page > EGB_SMMU_SIZE) continue;
        egb_rd(io, r, EGB_WIN_SMMU, page + 4 * cb, &r->sid_cbar[s]);
        egb_rd(io, r, EGB_WIN_SMMU, cb_base + cb * page, &r->sid_sctlr[s]);
    }
}

// Runs the gated sequence. `have` is a bitmask of mapped windows (1 << EGB_WIN_*); a missing window makes its stage EGB_NOMAP.
static inline void egb_run(const struct egb_io *io, unsigned mode, unsigned have, struct egb_result *r) {
    __builtin_memset(r, 0, sizeof *r);
    r->mode = mode;
    r->gx = r->smmu = r->cc = EGB_NOT_RUN;
    if (!mode) {r->gcc = EGB_SKIPPED_MODE; return;}
    // Independent reads: the speed-bin fuse word and the command-DB header (reserved DRAM).
    if (!(have & (1u << EGB_WIN_FUSE))) r->fuse = EGB_NOMAP;
    else if (!egb_rd(io, r, EGB_WIN_FUSE, EGB_FUSE_SPEED_BIN, &r->speed_bin_word)) r->fuse = EGB_DONE;
    if (!(have & (1u << EGB_WIN_CMDDB))) r->cmddb = EGB_NOMAP;
    else if (!egb_rd(io, r, EGB_WIN_CMDDB, 0, &r->cmddb_version) && !egb_rd(io, r, EGB_WIN_CMDDB, 4, &r->cmddb_magic)) r->cmddb = EGB_DONE;
    // Stage 1: GCC GPU branches.
    if (!(have & (1u << EGB_WIN_GCC_GPU))) {r->gcc = EGB_NOMAP; return;}
    if (egb_rd(io, r, EGB_WIN_GCC_GPU, EGB_GCC_GPU_CFG_AHB, &r->cfg_ahb)) return;
    egb_rd(io, r, EGB_WIN_GCC_GPU, EGB_GCC_GPU_GEMNOC_GFX, &r->gemnoc);
    egb_rd(io, r, EGB_WIN_GCC_GPU, EGB_GCC_DDRSS_GPU_AXI, &r->axi);
    if (have & (1u << EGB_WIN_GCC_VOTE)) egb_rd(io, r, EGB_WIN_GCC_VOTE, EGB_GCC_PLL_VOTE, &r->pll_vote);
    r->cfg_ahb_after = r->cfg_ahb;
    if (!egb_cfg_ahb_enabled(r->cfg_ahb) && mode >= 2 &&
        !egb_wr(io, r, EGB_WIN_GCC_GPU, EGB_GCC_GPU_CFG_AHB, r->cfg_ahb | EGB_CBCR_ENABLE)) {
        r->enabled_ahb = 1;
        egb_rd(io, r, EGB_WIN_GCC_GPU, EGB_GCC_GPU_CFG_AHB, &r->cfg_ahb_after);
    }
    r->gcc = EGB_DONE;
    // Stage 2: GPUCC needs gcc_gpu_cfg_ahb_clk.
    if (!(have & (1u << EGB_WIN_GPUCC))) {r->cc = EGB_NOMAP; return;}
    if (!egb_cfg_ahb_enabled(r->cfg_ahb_after)) {r->cc = EGB_SKIPPED_PREREQ; return;}
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDSCR, &r->cx_gdscr);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GDS_HW_CTRL, &r->cx_hwctrl);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_AHB_CLK, &r->cc_ahb);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_SMMU_VOTE_CLK, &r->cc_smmu);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CX_GMU_CLK, &r->cc_gmu);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_CXO_CLK, &r->cc_cxo);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_HUB_CX_INT_CLK, &r->cc_hub);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_GX_GMU_CLK, &r->cc_gx_gmu);
    egb_rd(io, r, EGB_WIN_GPUCC, EGB_CC_GX_BCR, &r->gx_bcr);
    if (r->io_error) return;
    r->cc = EGB_DONE;
    r->cx_before = r->cx_gdscr;
    r->cc_smmu_before = r->cc_smmu;
    // CX is on when the hardware controller reports it (VOTABLE: another master's vote counts) or the GDSCR itself does.
    int cx_on = egb_gdsc_on(r->cx_hwctrl) || egb_gdsc_on(r->cx_gdscr);
    if (mode >= 3 && !cx_on) {
        egb_power_cx(io, r);
        cx_on = (int)r->powered_cx;
    }
    if (mode >= 3 && cx_on && !egb_clock_on(r->cc_smmu)) egb_smmu_clocks(io, r);
    // Stage 3: the GX GDSC in gxclkctl (its power domain is the CX GDSC). Read only.
    if (!(have & (1u << EGB_WIN_GXCLK))) r->gx = EGB_NOMAP;
    else if (!cx_on) r->gx = EGB_SKIPPED_PREREQ;
    else {
        egb_rd(io, r, EGB_WIN_GXCLK, EGB_GX_GDSCR, &r->gx_gdscr);
        egb_rd(io, r, EGB_WIN_GXCLK, EGB_GX_CFG_GDSCR, &r->gx_cfg);
        r->gx = EGB_DONE;
    }
    // Stage 4: the adreno SMMU needs CX and its HLOS vote clock (DT: power-domains CX GDSC, clocks gpu_cc_hlos1_vote_gpu_smmu).
    if (!(have & (1u << EGB_WIN_SMMU))) r->smmu = EGB_NOMAP;
    else if (r->io_error || !cx_on || !egb_clock_on(r->cc_smmu)) r->smmu = EGB_SKIPPED_PREREQ;
    else {
        egb_read_smmu(io, r);
        r->smmu = EGB_DONE;
    }
}

static inline const char *egb_state_text(enum egb_state s) {
    return s == EGB_DONE ? "ok" : s == EGB_SKIPPED_PREREQ ? "skip" : s == EGB_SKIPPED_MODE ? "off" : s == EGB_NOMAP ? "nomap" : "-";
}
#endif
