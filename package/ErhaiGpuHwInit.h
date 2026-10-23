/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ERHAI_GPU_HW_INIT_H
#define ERHAI_GPU_HW_INIT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The register part of the Adreno 830 hw_init (a8xx_gpu.c), in three pieces around the ring work the caller does with
 * NativeAdreno (ring attach, CP_ME_INIT, zap and CP_SET_SECURE_MODE):
 *   Pre      after OOB GPU_SET: aperture clear through CP_SQE_INSTR_BASE, including adreno_hw_init's SMMU aperture call;
 *   StartCp  after the ring attach programmed CP_RB_*: CP_RB_RPTR_ADDR_BV and CP_SQE_CNTL = 1;
 *   Post     after the secure-mode switch: RB_GC_GMEM_PROTECT, the power-up register list, hardware clock gating and
 *            the OOB GPU_SET clear.
 * Registers are dword offsets from the GPU base (0x3d00000); the GMU's sit in the same numbering (its window starts at
 * GPU + 0x6c000). Values, lists and masks come from build/erhai-gpu/ErhaiGpuSource.h (prepare-erhai-gpu.py). The
 * caller holds OOB GPU_SET with GX on, and owns the ring, its buffers and the pwrup page. Nothing here retries. */
typedef struct {
    void *cookie;
    int (*write32)(void *, uint32_t reg, uint32_t value);
    int (*read32)(void *, uint32_t reg, uint32_t *value);
    int (*wait_us)(void *, unsigned us);
    /* qcom_scm_set_gpu_smmu_aperture(0) when the call is available; its result is recorded, never fatal (Linux logs it). */
    int (*smmu_aperture)(void *);
} ErhaiGpuIo;
typedef enum {ErhaiGpuOk, ErhaiGpuArgument, ErhaiGpuIoError, ErhaiGpuTimeout} ErhaiGpuStatus;
typedef struct {
    uint64_t sqe_iova;       /* CP_SQE_INSTR_BASE: the SQE microcode without its first dword */
    uint64_t rptr_bv_iova;   /* CP_RB_RPTR_ADDR_BV: rbmemptr(bv_rptr) */
} ErhaiGpuHwConfig;
typedef struct {
    uint32_t aperture;       /* a6xx_gpu->cached_aperture */
    uint32_t gmem_protect;   /* RB_GC_GMEM_PROTECT from the nonctxt list */
    uint32_t step, failed_reg;
    int smmu_aperture;       /* the SCM result, or 1 when no callback */
    uint32_t p2s_polls, p2s_status;
    uint32_t pwrup_ifpc, pwrup_preemption, pwrup_dynamic;
} ErhaiGpuHw;
ErhaiGpuStatus ErhaiGpuHwInitPre(ErhaiGpuHw *, const ErhaiGpuIo *, const ErhaiGpuHwConfig *);
ErhaiGpuStatus ErhaiGpuHwInitStartCp(ErhaiGpuHw *, const ErhaiGpuIo *, const ErhaiGpuHwConfig *);
/* pwrup: the CPU view of the power-up list page (struct cpu_gpu_lock and the lists after it). */
ErhaiGpuStatus ErhaiGpuHwInitPost(ErhaiGpuHw *, const ErhaiGpuIo *, volatile uint32_t *pwrup, size_t pwrup_bytes,
                                  size_t *pwrup_written);
/* For the board's rows: RBBM_STATUS, RBBM_INT_0_STATUS, CP_INTERRUPT_STATUS_GLOBAL, CP_HW_FAULT_STATUS_PIPE (through the
 * aperture as left, PIPE_NONE after each step), CP_RB_RPTR, CP_RB_WPTR; 0xdeadbeef where a read failed. */
void ErhaiGpuHwSnapshot(const ErhaiGpuIo *, uint32_t out[6]);
/* The ring words hw_init sends once the zap shader is up: CP_SET_SECURE_MODE with 0 (leave secure mode). Zero on success. */
int ErhaiGpuSecureModePacket(uint32_t words[2]);
/* The facts the user client reports to the Vulkan driver, from the pinned sources: the a6xx_catalog.c A830 entry (chip id,
 * gmem), the GMEM window hw_init programs (UCHE_CCHE/SP_HLSQ GMEM_RANGE_MIN), uche_trap_base, and sm8750's UBWC configuration
 * (ubwc_config.c ubwc_5_0_hbb16 through ubwc.h: bank bit, swizzle levels, macrotile mode, encoder major version). */
typedef struct {
    uint32_t chip_id, gmem_bytes;
    uint64_t gmem_base, uche_trap_base;
    uint32_t highest_bank_bit, ubwc_swizzle, macrotile_mode, ubwc_version;
} ErhaiGpuFacts;
void ErhaiGpuDeviceFacts(ErhaiGpuFacts *);
#ifdef __cplusplus
}
#endif
#endif
