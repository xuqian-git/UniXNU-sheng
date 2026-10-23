// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the erhai gated GPU power / SMMU observation sequence (software register peer, not hardware).
#include "ErhaiGpuBoard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct model {
    uint32_t ahb, gemnoc, axi, vote, cx, hwctrl, ccahb, ccsmmu, ccgmu, gx, gxcfg, fuse, db[2];
    uint32_t id0, id1, id2, smr[128], s2cr[128], cbar[64], sctlr[64];
    unsigned stuck_cx, cx_writes, gcc_writes, smmu_reads, cb_reads;
    int deny_smmu, deny_gx, violations, fail_write;
};
static uint32_t cb_base(const struct model *m) {return egb_smmu_cb_base(m->id1);}
static int rd(void *c, unsigned w, uint32_t o, uint32_t *out) {
    struct model *m = c;
    uint32_t v = 0xdead0000u | w;
    switch (w) {
    case EGB_WIN_GCC_GPU: v = o == EGB_GCC_GPU_CFG_AHB ? m->ahb : o == EGB_GCC_GPU_GEMNOC_GFX ? m->gemnoc : o == EGB_GCC_DDRSS_GPU_AXI ? m->axi : v; break;
    case EGB_WIN_GCC_VOTE: v = m->vote; break;
    case EGB_WIN_GPUCC:
        if (!(m->ahb & EGB_CBCR_ENABLE)) m->violations++;     // GPUCC read with the GCC AHB branch not enabled
        v = o == EGB_CC_CX_GDSCR ? m->cx : o == EGB_CC_CX_GDS_HW_CTRL ? m->hwctrl : o == EGB_CC_AHB_CLK ? m->ccahb :
            o == EGB_CC_SMMU_VOTE_CLK ? m->ccsmmu : o == EGB_CC_CX_GMU_CLK ? m->ccgmu : 0x80000000u;
        break;
    case EGB_WIN_GXCLK:
        if (m->deny_gx || !(m->hwctrl & EGB_GDSC_PWR_ON)) m->violations++;
        v = o == EGB_GX_GDSCR ? m->gx : o == EGB_GX_CFG_GDSCR ? m->gxcfg : v;
        break;
    case EGB_WIN_SMMU: {
        if (m->deny_smmu || !(m->hwctrl & EGB_GDSC_PWR_ON) || (m->ccsmmu & EGB_CBCR_CLK_OFF)) m->violations++;
        m->smmu_reads++;
        const uint32_t page = egb_smmu_page(m->id1);
        if (o == EGB_SMMU_ID0) v = m->id0;
        else if (o == EGB_SMMU_ID1) v = m->id1;
        else if (o == EGB_SMMU_ID2) v = m->id2;
        else if (o >= 0x800 && o < 0xa00) v = m->smr[(o - 0x800) / 4];
        else if (o >= 0xc00 && o < 0xe00) v = m->s2cr[(o - 0xc00) / 4];
        else if (o >= page && o < page + 0x100) v = m->cbar[(o - page) / 4];
        else if (o >= cb_base(m)) {m->cb_reads++; v = m->sctlr[(o - cb_base(m)) / page];}
        else if (o < 0x100) v = 0;
        break;
    }
    case EGB_WIN_FUSE: v = o == EGB_FUSE_SPEED_BIN ? m->fuse : v; break;
    case EGB_WIN_CMDDB: v = m->db[o / 4]; break;
    default: return -1;
    }
    *out = v;
    return 0;
}
static int wr(void *c, unsigned w, uint32_t o, uint32_t v) {
    struct model *m = c;
    if (m->fail_write) return -1;
    if (w == EGB_WIN_GCC_GPU && o == EGB_GCC_GPU_CFG_AHB) {m->gcc_writes++; m->ahb = v & ~EGB_CBCR_CLK_OFF; return 0;}
    if (w != EGB_WIN_GPUCC) {m->violations++; return -1;}
    if (o == EGB_CC_CX_GDSCR) {
        m->cx_writes++;
        m->cx = v;
        if (!(v & EGB_GDSC_SW_COLLAPSE) && !m->stuck_cx) {m->hwctrl = EGB_GDSC_PWR_ON; m->cx |= EGB_GDSC_PWR_ON;}
        return 0;
    }
    if (o == EGB_CC_AHB_CLK) {m->ccahb = v; return 0;}
    if (o == EGB_CC_SMMU_VOTE_CLK) {m->ccsmmu = (v & EGB_CBCR_ENABLE) ? v & ~EGB_CBCR_CLK_OFF : v; return 0;}
    m->violations++;
    return -1;
}
#define CHECK(c) do { if (!(c)) {fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1);} } while (0)
#define ALL ((1u << EGB_WINDOWS) - 1)

static struct model base(void) {
    struct model m; memset(&m, 0, sizeof m);
    m.ahb = EGB_CBCR_CLK_OFF | EGB_CBCR_ENABLE;          // hwcg branch: idle CLK_OFF with the enable bit set
    m.gemnoc = m.axi = EGB_CBCR_CLK_OFF;
    m.cx = EGB_GDSC_SW_COLLAPSE; m.ccsmmu = EGB_CBCR_CLK_OFF; m.ccahb = EGB_CBCR_CLK_OFF;
    m.id0 = 0x0000a020; m.id1 = 0x10000018; m.id2 = 0x0000005e;   // 32 SMRs, 24 banks, NUMPAGENDXB 1 (4 pages), 4 KiB pages
    m.fuse = 0x0000003e; m.db[0] = 1; m.db[1] = EGB_CMDDB_MAGIC;
    return m;
}
static struct egb_result run(struct model *m, unsigned mode, unsigned have) {
    struct egb_io io = {m, rd, wr, 0};
    struct egb_result r;
    egb_run(&io, mode, have, &r);
    return r;
}

int main(void) {
    // Mode 1, CX off: GCC and GPUCC read, GX and SMMU skipped, nothing written; fuse and command DB read.
    struct model m = base();
    struct egb_result r = run(&m, 1, ALL);
    CHECK(r.gcc == EGB_DONE && r.cc == EGB_DONE && r.gx == EGB_SKIPPED_PREREQ && r.smmu == EGB_SKIPPED_PREREQ);
    CHECK(!m.cx_writes && !m.gcc_writes && !m.smmu_reads && !m.violations && !r.io_error);
    CHECK(r.fuse == EGB_DONE && (r.speed_bin_word & 0x1ff) == 0x3e && r.cmddb == EGB_DONE && r.cmddb_magic == EGB_CMDDB_MAGIC);
    // Mode 1 with the GCC AHB branch disabled: GPUCC is not read at all.
    m = base(); m.ahb = EGB_CBCR_CLK_OFF;
    r = run(&m, 1, ALL);
    CHECK(r.cc == EGB_SKIPPED_PREREQ && !m.violations && !m.gcc_writes);
    // Mode 2 enables it (the gcc probe's write) and then reads GPUCC.
    r = run(&m, 2, ALL);
    CHECK(r.enabled_ahb == 1 && m.gcc_writes == 1 && r.cc == EGB_DONE && !m.violations && !m.cx_writes);
    // Mode 3: Linux's gdsc_init + gdsc_enable for the CX GDSC, then the SMMU clocks, then the SMMU and GX reads.
    m = base();
    m.smr[3] = 0x80000000u | 0x5; m.s2cr[3] = 0x00000001;          // SID 5 -> bank 1, translate
    m.smr[7] = 0x80000000u | (0x1u << 16); m.s2cr[7] = 0x00000000;  // SIDs 0/1 (mask 1) -> bank 0
    m.smr[9] = 0x80000002u; m.s2cr[9] = 0x00020000;                 // SID 2 fault (secure, not ours)
    m.cbar[0] = 0x00000a00; m.cbar[1] = 0x00000b01; m.sctlr[0] = 0x67; m.sctlr[1] = 0x1;
    m.gx = 0x80000000u; m.gxcfg = EGB_CFG_POWER_UP_COMPLETE;
    r = run(&m, 3, ALL);
    CHECK(r.powered_cx && r.powered_clocks && !m.violations && !r.io_error);
    CHECK((m.cx & EGB_GDSC_INIT_MASK) == EGB_GDSC_CX_WAITS && (m.cx & EGB_GDSC_RETAIN_FF) && !(m.cx & EGB_GDSC_SW_COLLAPSE));
    CHECK(r.smmu == EGB_DONE && r.gx == EGB_DONE && r.gx_cfg == EGB_CFG_POWER_UP_COMPLETE);
    CHECK(r.num_smr == 32 && r.num_cb == 24 && r.smr_valid == 3 && r.types[0] == 2 && r.types[2] == 1 && r.shown == 3);
    CHECK(r.sid_slot[0] == 7 && r.sid_slot[1] == 7 && r.sid_cb[0] == 0 && r.sid_slot[2] == 9 && r.sid_type[2] == 2);
    CHECK(r.sid_slot[3] == 3 && r.sid_cb[3] == 1 && r.sid_cbar[3] == 0xb01 && r.sid_sctlr[3] == 0x1 && r.sid_sctlr[0] == 0x67);
    CHECK(r.sid_slot[4] == EGB_NO_MATCH && r.sid_cbar[2] == 0 && m.cb_reads == 3);   // fault entries' banks are not read
    // Mode 3 with CX already on (another master's vote): no GDSC write, the clocks only if the vote clock is off.
    m = base(); m.hwctrl = EGB_GDSC_PWR_ON; m.cx = EGB_GDSC_PWR_ON; m.ccsmmu = 1;
    r = run(&m, 3, ALL);
    CHECK(!m.cx_writes && !r.powered_cx && !r.powered_clocks && r.smmu == EGB_DONE && !m.violations);
    // Mode 3 with a CX GDSC that never reports on: no clock write, no SMMU or GX read.
    m = base(); m.stuck_cx = 1;
    r = run(&m, 3, ALL);
    CHECK(!r.powered_cx && r.smmu == EGB_SKIPPED_PREREQ && r.gx == EGB_SKIPPED_PREREQ && !m.smmu_reads && !m.violations);
    CHECK(m.cx_writes == 2 && !(m.ccsmmu & EGB_CBCR_ENABLE));
    // A failing write stops the power-on and leaves the SMMU unread.
    m = base(); m.fail_write = 1;
    r = run(&m, 3, ALL);
    CHECK(r.io_error && !r.powered_cx && r.smmu == EGB_SKIPPED_PREREQ && !m.smmu_reads);
    // Missing windows: GCC unmapped stops the chain; SMMU unmapped reports nomap.
    m = base();
    r = run(&m, 3, ALL & ~(1u << EGB_WIN_GCC_GPU));
    CHECK(r.gcc == EGB_NOMAP && r.cc == EGB_NOT_RUN && !m.cx_writes);
    m = base();
    r = run(&m, 3, ALL & ~(1u << EGB_WIN_SMMU));
    CHECK(r.smmu == EGB_NOMAP && r.powered_cx && !m.smmu_reads);
    // Mode 0 does nothing.
    m = base();
    r = run(&m, 0, ALL);
    CHECK(r.gcc == EGB_SKIPPED_MODE && !m.cx_writes && !m.smmu_reads && r.fuse == EGB_NOT_RUN);
    // SMR matching: mask bits are don't-care, invalid entries never match.
    CHECK(egb_smr_match(0x80010000u, 1) && egb_smr_match(0x80010000u, 0) && !egb_smr_match(0x80010000u, 2) && !egb_smr_match(0x5, 5));
    CHECK(egb_smmu_cb_base(0x10000018) == 0x4000 && egb_smmu_cb_base(0x80000000u | (2u << 28)) == 0x80000);
    printf("PASS: 11 erhai GPU power/SMMU observation cases (gating, Linux CX GDSC sequence, SMR routing, failures, windows)\n");
    return 0;
}
