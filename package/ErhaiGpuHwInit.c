/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ErhaiGpuHwInit.h"
#include "ErhaiGpuSource.h"
#include "AdrenoPM4.h"

/* The order, values and conditions are those of hw_init and its helpers in the pinned msm-next a8xx_gpu.c, for the A830
 * catalog entry (one ring, no AQE image, IFPC/preemption lists programmed, the sm8750 UBWC 5.0 configuration).
 * scripts/check-erhai-gpu-hwinit.py compares every access with those functions built as a software oracle. */
#define CHECK(x) do {if ((x)) return ErhaiGpuIoError;} while (0)

static int wr(ErhaiGpuHw *hw, const ErhaiGpuIo *io, uint32_t reg, uint32_t value)
{
    if (io->write32(io->cookie, reg, value)) {hw->failed_reg = reg; return 1;}
    return 0;
}
static int rd(ErhaiGpuHw *hw, const ErhaiGpuIo *io, uint32_t reg, uint32_t *value)
{
    if (io->read32(io->cookie, reg, value)) {hw->failed_reg = reg; return 1;}
    return 0;
}
static int wr64(ErhaiGpuHw *hw, const ErhaiGpuIo *io, uint32_t reg, uint64_t value)
{
    return wr(hw, io, reg, (uint32_t)value) || wr(hw, io, reg + 1, (uint32_t)(value >> 32));
}
/* a8xx_aperture_slice_set: CP_APERTURE_CNTL_HOST is written only when the pipe/slice selection changes. */
static int aperture(ErhaiGpuHw *hw, const ErhaiGpuIo *io, uint32_t pipe, uint32_t slice)
{
    const uint32_t value = (pipe << ERHAI_GPU_CP_APERTURE_PIPEID_SHIFT) | (slice << ERHAI_GPU_CP_APERTURE_SLICEID_SHIFT);
    if (hw->aperture == value) return 0;
    if (wr(hw, io, ERHAI_GPU_CP_APERTURE_CNTL_HOST, value)) return 1;
    hw->aperture = value;
    return 0;
}
static int write_pipe(ErhaiGpuHw *hw, const ErhaiGpuIo *io, uint32_t pipe, uint32_t reg, uint32_t value)
{
    return aperture(hw, io, pipe, 0) || wr(hw, io, reg, value);
}
/* a8xx_set_ubwc_config with the sm8750 data (UBWC 5.0, highest bank bit 16). */
static int ubwc(ErhaiGpuHw *hw, const ErhaiGpuIo *io)
{
    const uint32_t version = ERHAI_GPU_UBWC_ENC_VERSION;
    const uint32_t lvl2_dis = !(ERHAI_GPU_UBWC_SWIZZLE & ERHAI_GPU_UBWC_SWIZZLE_LVL2);
    const uint32_t lvl3_dis = !(ERHAI_GPU_UBWC_SWIZZLE & ERHAI_GPU_UBWC_SWIZZLE_LVL3);
    const uint32_t yuvnotcomptofc = version == ERHAI_GPU_UBWC_6_0;
    const uint32_t rgba8888_lossless = version < ERHAI_GPU_UBWC_5_0 && version >= ERHAI_GPU_UBWC_4_0;
    const uint32_t fp16compoptdis = version < ERHAI_GPU_UBWC_4_3;
    const uint32_t rgb565_predicator = version >= ERHAI_GPU_UBWC_4_0;
    const uint32_t amsbc = ERHAI_GPU_UBWC_AMSBC, mode = ERHAI_GPU_UBWC_VERSION_TAG, ubwc_mode = ERHAI_GPU_UBWC_MODE;
    const uint32_t min_acc_len_64b = 0, uavflagprd_inv = 2;
    const uint32_t hbb = ERHAI_GPU_UBWC_HIGHEST_BANK_BIT - 13, hbb_hi = hbb >> 2, hbb_lo = hbb & 3;
    const uint32_t gras = hbb << 5 | lvl3_dis << 4 | lvl2_dis << 3;
    return write_pipe(hw, io, ERHAI_GPU_PIPE_BV, ERHAI_GPU_GRAS_NC_MODE_CNTL, gras) ||
           write_pipe(hw, io, ERHAI_GPU_PIPE_BR, ERHAI_GPU_GRAS_NC_MODE_CNTL, gras) ||
           write_pipe(hw, io, ERHAI_GPU_PIPE_BR, ERHAI_GPU_RB_CCU_NC_MODE_CNTL,
                      yuvnotcomptofc << 6 | lvl3_dis << 5 | lvl2_dis << 4 | hbb_hi << 3 | hbb_lo << 1) ||
           write_pipe(hw, io, ERHAI_GPU_PIPE_BR, ERHAI_GPU_RB_CMP_NC_MODE_CNTL,
                      mode << 15 | yuvnotcomptofc << 6 | rgba8888_lossless << 4 | fp16compoptdis << 3 |
                      rgb565_predicator << 2 | amsbc << 1 | min_acc_len_64b) ||
           aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0) ||
           wr(hw, io, ERHAI_GPU_SP_NC_MODE_CNTL, lvl3_dis << 13 | lvl2_dis << 12 | hbb_hi << 10 | uavflagprd_inv << 4 |
                                                 min_acc_len_64b << 3 | hbb_lo << 1 | ubwc_mode) ||
           wr(hw, io, ERHAI_GPU_TPL1_NC_MODE_CNTL, lvl3_dis << 7 | lvl2_dis << 6 | hbb_hi << 4 | min_acc_len_64b << 3 |
                                                   hbb_lo << 1 | ubwc_mode);
}
/* a8xx_nonctxt_config: every pipe but LPAC, each catalog entry whose pipe mask names it. */
static int nonctxt(ErhaiGpuHw *hw, const ErhaiGpuIo *io)
{
    for (uint32_t pipe = ERHAI_GPU_PIPE_NONE; pipe <= ERHAI_GPU_PIPE_DDE_BV; ++pipe) {
        if (pipe == ERHAI_GPU_PIPE_LPAC) continue;
        if (aperture(hw, io, pipe, 0)) return 1;
        for (unsigned i = 0; i < ERHAI_GPU_NONCTXT_COUNT; ++i) {
            if (!(erhai_gpu_NONCTXT[i][2] & (1u << pipe))) continue;
            if (erhai_gpu_NONCTXT[i][0] == ERHAI_GPU_RB_GC_GMEM_PROTECT) hw->gmem_protect = erhai_gpu_NONCTXT[i][1];
            if (wr(hw, io, erhai_gpu_NONCTXT[i][0], erhai_gpu_NONCTXT[i][1])) return 1;
        }
    }
    return aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0);
}
/* a8xx_set_cp_protect: the per-pipe control, the global list (zero entries skipped), the last span on PROTECT_PIPE(15). */
static int protect(ErhaiGpuHw *hw, const ErhaiGpuIo *io)
{
    uint32_t final_cfg = 0;
    if (write_pipe(hw, io, ERHAI_GPU_PIPE_BR, ERHAI_GPU_CP_PROTECT_CNTL_PIPE, ERHAI_GPU_CP_PROTECT_CNTL) ||
        write_pipe(hw, io, ERHAI_GPU_PIPE_BV, ERHAI_GPU_CP_PROTECT_CNTL_PIPE, ERHAI_GPU_CP_PROTECT_CNTL) ||
        aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0))
        return 1;
    for (unsigned i = 0; i < ERHAI_GPU_PROTECT_COUNT; ++i) {
        if (!erhai_gpu_PROTECT[i]) continue;
        if (wr(hw, io, ERHAI_GPU_CP_PROTECT_GLOBAL_0 + i * ERHAI_GPU_CP_PROTECT_GLOBAL_STRIDE, erhai_gpu_PROTECT[i])) return 1;
        final_cfg = erhai_gpu_PROTECT[i];
    }
    return write_pipe(hw, io, ERHAI_GPU_PIPE_BR, ERHAI_GPU_CP_PROTECT_PIPE_15, final_cfg) ||
           write_pipe(hw, io, ERHAI_GPU_PIPE_BV, ERHAI_GPU_CP_PROTECT_PIPE_15, final_cfg) ||
           aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0);
}
ErhaiGpuStatus ErhaiGpuHwInitPre(ErhaiGpuHw *hw, const ErhaiGpuIo *io, const ErhaiGpuHwConfig *config)
{
    if (!hw || !io || !io->write32 || !io->read32 || !io->wait_us || !config || !config->sqe_iova || (config->sqe_iova & 4095))
        return ErhaiGpuArgument;
    *hw = (ErhaiGpuHw){.aperture = UINT32_MAX, .smmu_aperture = 1};
    uint32_t v = 0;
    hw->step = 1;
    CHECK(aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0));
    /* GBIF halts released in case GX was not collapsed; the secure TSB range disabled. */
    CHECK(wr(hw, io, ERHAI_GPU_GBIF_HALT, 0) || rd(hw, io, ERHAI_GPU_GBIF_HALT, &v));
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_GBIF_HALT, 0) || rd(hw, io, ERHAI_GPU_RBBM_GBIF_HALT, &v));
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_SECVID_TSB_CNTL, 0) || wr64(hw, io, ERHAI_GPU_RBBM_SECVID_TSB_TRUSTED_BASE, 0) ||
          wr(hw, io, ERHAI_GPU_RBBM_SECVID_TSB_TRUSTED_SIZE, 0));
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff));
    hw->step = 2;
    /* GMEM at 64 MiB in UCHE/CCHE and SP/HLSQ, the UCHE trap region at uche_trap_base. */
    CHECK(wr64(hw, io, ERHAI_GPU_UCHE_CCHE_GC_GMEM_RANGE_MIN, ERHAI_GPU_GMEM_RANGE_MIN_VALUE) ||
          wr64(hw, io, ERHAI_GPU_SP_HLSQ_GC_GMEM_RANGE_MIN, ERHAI_GPU_GMEM_RANGE_MIN_VALUE));
    CHECK(wr64(hw, io, ERHAI_GPU_UCHE_TRAP_BASE, ERHAI_GPU_UCHE_TRAP_BASE_VALUE) ||
          wr64(hw, io, ERHAI_GPU_UCHE_WRITE_THRU_BASE, ERHAI_GPU_UCHE_TRAP_BASE_VALUE) ||
          wr64(hw, io, ERHAI_GPU_UCHE_CCHE_TRAP_BASE, ERHAI_GPU_UCHE_TRAP_BASE_VALUE) ||
          wr64(hw, io, ERHAI_GPU_UCHE_CCHE_WRITE_THRU_BASE, ERHAI_GPU_UCHE_TRAP_BASE_VALUE));
    /* Performance counters, the IFPC counter (countable 4 on XOCLK1), CP0 counting cycles. */
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_PERFCTR_CNTL, 1) || wr(hw, io, ERHAI_GPU_RBBM_SLICE_PERFCTR_CNTL, 1));
    CHECK(wr(hw, io, ERHAI_GPU_GMU_POWER_COUNTER_SELECT_XOCLK_1, 0x4));
    CHECK(wr(hw, io, ERHAI_GPU_CP_PERFCTR_CP_SEL_0, 1));
    hw->step = 3;
    CHECK(ubwc(hw, io));
    for (unsigned i = 0; i < 20; ++i)
        CHECK(wr(hw, io, ERHAI_GPU_TPL1_BICUBIC_WEIGHTS_TABLE_0 + i * ERHAI_GPU_TPL1_BICUBIC_WEIGHTS_TABLE_STRIDE,
                 erhai_gpu_BICUBIC[i]));
    CHECK(wr(hw, io, ERHAI_GPU_UCHE_CLIENT_PF, (1u << 7) | 0x1));
    hw->step = 4;
    CHECK(nonctxt(hw, io));
    /* Fault detection, the CX busy counter, the GMU power counter. */
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0xcfffff));
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_SLICE_INTERFACE_HANG_INT_CNTL, 1u << 30));
    CHECK(wr(hw, io, ERHAI_GPU_GMU_AO_GPU_CX_BUSY_MASK, 0xff000000));
    CHECK(rd(hw, io, ERHAI_GPU_GMU_POWER_COUNTER_SELECT_XOCLK_0, &v) ||
          wr(hw, io, ERHAI_GPU_GMU_POWER_COUNTER_SELECT_XOCLK_0, (v & ~0xffu) | (1u << 5)));
    CHECK(wr(hw, io, ERHAI_GPU_GMU_POWER_COUNTER_ENABLE, 1));
    hw->step = 5;
    CHECK(protect(hw, io));
    CHECK(write_pipe(hw, io, ERHAI_GPU_PIPE_BR, ERHAI_GPU_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE, 1));
    for (uint32_t pipe = ERHAI_GPU_PIPE_BR; pipe <= ERHAI_GPU_PIPE_DDE_BV; ++pipe) {
        if (pipe == ERHAI_GPU_PIPE_LPAC) continue;
        CHECK(aperture(hw, io, pipe, 0) ||
              wr(hw, io, ERHAI_GPU_CP_APRIV_CNTL_PIPE, pipe == ERHAI_GPU_PIPE_BR ? ERHAI_GPU_BR_APRIV_MASK : ERHAI_GPU_APRIV_MASK) ||
              wr(hw, io, ERHAI_GPU_CP_INTERRUPT_STATUS_MASK_PIPE, ERHAI_GPU_CP_INTERRUPT_STATUS_MASK_PIPE_VALUE) ||
              wr(hw, io, ERHAI_GPU_CP_HW_FAULT_STATUS_MASK_PIPE, ERHAI_GPU_CP_HW_FAULT_STATUS_MASK_PIPE_VALUE));
    }
    CHECK(aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0));
    CHECK(wr(hw, io, ERHAI_GPU_CP_INTERRUPT_STATUS_MASK_GLOBAL, ERHAI_GPU_CP_GLOBAL_INT_MASK));
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_INT_0_MASK, ERHAI_GPU_INT_MASK));
    hw->step = 6;
    /* adreno_hw_init: the SMMU aperture for context bank 0 (the ring memory pointers are the caller's). */
    if (io->smmu_aperture) hw->smmu_aperture = io->smmu_aperture(io->cookie);
    CHECK(wr64(hw, io, ERHAI_GPU_CP_SQE_INSTR_BASE, config->sqe_iova));
    hw->step = 7;
    return ErhaiGpuOk;
}
ErhaiGpuStatus ErhaiGpuHwInitStartCp(ErhaiGpuHw *hw, const ErhaiGpuIo *io, const ErhaiGpuHwConfig *config)
{
    if (!hw || hw->step != 7 || !io || !config || !config->rptr_bv_iova) return ErhaiGpuArgument;
    /* After CP_RB_BASE/CNTL/RPTR_ADDR: the BV read-pointer shadow; a8xx_preempt_hw_init does nothing with one ring. */
    CHECK(wr64(hw, io, ERHAI_GPU_CP_RB_RPTR_ADDR_BV, config->rptr_bv_iova));
    CHECK(wr(hw, io, ERHAI_GPU_CP_SQE_CNTL, 1));
    hw->step = 8;
    return ErhaiGpuOk;
}
/* a8xx_patch_pwrup_reglist: the lock header, the IFPC and preemption lists as (offset, value) pairs, then the dynamic list
 * as (aperture, offset, value) triplets read through each pipe's aperture on slice 0. */
static int pwrup_list(ErhaiGpuHw *hw, const ErhaiGpuIo *io, volatile uint32_t *page, size_t bytes, size_t *written)
{
    unsigned dynamic = 0;
    for (uint32_t pipe = ERHAI_GPU_PIPE_BR; pipe <= ERHAI_GPU_PIPE_DDE_BV; ++pipe)
        for (unsigned i = 0; i < ERHAI_GPU_DYN_PWRUP_COUNT; ++i) dynamic += !!(erhai_gpu_DYN_PWRUP[i][1] & (1u << pipe));
    const size_t need = 16 + (ERHAI_GPU_IFPC_COUNT + ERHAI_GPU_PWRUP_COUNT) * 8 + dynamic * 12;
    if (need > bytes || ERHAI_GPU_IFPC_COUNT > 255 || ERHAI_GPU_PWRUP_COUNT > 255) return 1;
    page[0] = page[1] = page[2] = 0;
    volatile uint32_t *dest = page + 4;
    uint32_t v = 0;
    for (unsigned i = 0; i < ERHAI_GPU_IFPC_COUNT; ++i) {
        if (rd(hw, io, erhai_gpu_IFPC[i], &v)) return 1;
        *dest++ = erhai_gpu_IFPC[i]; *dest++ = v;
    }
    for (unsigned i = 0; i < ERHAI_GPU_PWRUP_COUNT; ++i) {
        if (rd(hw, io, erhai_gpu_PWRUP[i], &v)) return 1;
        *dest++ = erhai_gpu_PWRUP[i]; *dest++ = v;
    }
    unsigned count = 0;
    for (uint32_t pipe = ERHAI_GPU_PIPE_BR; pipe <= ERHAI_GPU_PIPE_DDE_BV; ++pipe)
        for (unsigned i = 0; i < ERHAI_GPU_DYN_PWRUP_COUNT; ++i) {
            if (!(erhai_gpu_DYN_PWRUP[i][1] & (1u << pipe))) continue;
            if (aperture(hw, io, pipe, 0) || rd(hw, io, erhai_gpu_DYN_PWRUP[i][0], &v)) return 1;
            *dest++ = pipe << ERHAI_GPU_CP_APERTURE_PIPEID_SHIFT; *dest++ = erhai_gpu_DYN_PWRUP[i][0]; *dest++ = v;
            ++count;
        }
    /* ifpc_list_len (u8), preemption_list_len (u8), dynamic_list_len (u16) share the fourth word, little-endian. */
    page[3] = ERHAI_GPU_IFPC_COUNT | ERHAI_GPU_PWRUP_COUNT << 8 | count << 16;
    hw->pwrup_ifpc = ERHAI_GPU_IFPC_COUNT; hw->pwrup_preemption = ERHAI_GPU_PWRUP_COUNT; hw->pwrup_dynamic = count;
    *written = (size_t)((const volatile uint8_t *)dest - (const volatile uint8_t *)page);
    return aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0);
}
ErhaiGpuStatus ErhaiGpuHwInitPost(ErhaiGpuHw *hw, const ErhaiGpuIo *io, volatile uint32_t *pwrup, size_t pwrup_bytes,
                                  size_t *pwrup_written)
{
    if (!hw || hw->step != 8 || !io || !pwrup || !pwrup_written || !hw->gmem_protect) return ErhaiGpuArgument;
    *pwrup_written = 0;
    hw->step = 9;
    /* RB_GC_GMEM_PROTECT only after the GPU left secure mode. */
    CHECK(write_pipe(hw, io, ERHAI_GPU_PIPE_BR, ERHAI_GPU_RB_GC_GMEM_PROTECT, hw->gmem_protect));
    CHECK(aperture(hw, io, ERHAI_GPU_PIPE_NONE, 0));
    hw->step = 10;
    if (pwrup_list(hw, io, pwrup, pwrup_bytes, pwrup_written)) return hw->failed_reg ? ErhaiGpuIoError : ErhaiGpuArgument;
    hw->step = 11;
    /* a8xx_set_hwcg(true): RBBM_CGC_0_PC for A830, the GMU AO CGC mode (no gmu_cgc_mode in the A830 entry), delay and
     * hysteresis, then the global load through the P2S transfer (TXDONE within 10 us). */
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_CGC_0_PC, 0x00000702));
    CHECK(wr(hw, io, ERHAI_GPU_GMU_AO_CGC_MODE_CNTL, 0) || wr(hw, io, ERHAI_GPU_GMU_AO_CGC_DELAY_CNTL, 0x110111) ||
          wr(hw, io, ERHAI_GPU_GMU_AO_CGC_HYST_CNTL, 0x55555));
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_CLOCK_CNTL_GLOBAL, 1) || wr(hw, io, ERHAI_GPU_RBBM_CGC_GLOBAL_LOAD_CMD, 1));
    CHECK(wr(hw, io, ERHAI_GPU_RBBM_CGC_P2S_TRIG_CMD, 1));
    /* A missing TXDONE ends a8xx_set_hwcg early (Linux logs it); hw_init still clears the OOB. */
    int done = 0;
    for (hw->p2s_polls = 1; hw->p2s_polls <= 11; ++hw->p2s_polls) {
        CHECK(rd(hw, io, ERHAI_GPU_RBBM_CGC_P2S_STATUS, &hw->p2s_status));
        if ((done = !!(hw->p2s_status & ERHAI_GPU_RBBM_CGC_P2S_STATUS_TXDONE))) break;
        if (io->wait_us(io->cookie, 1)) return ErhaiGpuIoError;
    }
    if (done) CHECK(wr(hw, io, ERHAI_GPU_RBBM_CLOCK_CNTL_GLOBAL, 0));
    hw->step = 12;
    /* a6xx_gmu_clear_oob(GMU_OOB_GPU_SET): the GMU may manage GPU power again. */
    CHECK(wr(hw, io, ERHAI_GPU_GMU_HOST2GMU_INTR_SET, 1u << 31));
    hw->step = 13;
    return ErhaiGpuOk;
}
int ErhaiGpuSecureModePacket(uint32_t words[2])
{
    if (!words || AdrenoPM4Type7(ERHAI_GPU_CP_SET_SECURE_MODE, 1, &words[0]) != AdrenoPM4Success) return 1;
    words[1] = 0;
    return 0;
}
void ErhaiGpuHwSnapshot(const ErhaiGpuIo *io, uint32_t out[6])
{
    static const uint32_t regs[6] = {ERHAI_GPU_RBBM_STATUS, ERHAI_GPU_RBBM_INT_0_STATUS, ERHAI_GPU_CP_INTERRUPT_STATUS_GLOBAL,
                                     ERHAI_GPU_CP_HW_FAULT_STATUS_PIPE, ERHAI_GPU_CP_RB_RPTR, ERHAI_GPU_CP_RB_WPTR};
    for (unsigned i = 0; i < 6; ++i)
        if (!io || !io->read32 || io->read32(io->cookie, regs[i], &out[i])) out[i] = 0xdeadbeef;
}
