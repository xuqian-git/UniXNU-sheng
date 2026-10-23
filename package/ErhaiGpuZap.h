/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ERHAI_GPU_ZAP_H
#define ERHAI_GPU_ZAP_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The zap shader load of a6xx_zap_shader_init -> zap_shader_load_mdt (adreno_gpu.c) for a single-file MBN: qcom_mdt_get_size,
 * qcom_mdt_load (metadata to PAS init_image, mem_setup when relocatable, the loadable segments into the reserved region)
 * and qcom_pas_auth_and_reset. Split images (.bNN files) are refused; there is no file system to fetch them from. Results
 * follow Linux: 0 or a negative errno, with the secure world's own errors already remapped by the platform. */
enum {ErhaiZapPasInitImage = 1, ErhaiZapPasMemSetup = 2, ErhaiZapPasAuthAndReset = 5};
typedef struct {
    void *cookie;
    /* One SIP SMC to service 2 (PIL): `ret ?: res.result[0]` as qcom_scm's PAS helpers return it. */
    int64_t (*pas)(void *, uint32_t cmd, uint32_t arginfo, const uint64_t *args, unsigned count);
    /* Make CPU writes to the buffer visible to the secure world (clean to PoC); zero on success. */
    int (*publish)(void *, const volatile void *ptr, size_t bytes);
} ErhaiZapIo;
typedef struct {
    /* The physically contiguous, 4 KiB aligned buffer init_image reads the metadata from (dma_alloc_coherent in Linux). */
    volatile uint8_t *metadata;
    uint64_t metadata_phys;
    size_t metadata_bytes;
    /* The zap-shader reserved-memory region (memremap'ed MEMREMAP_WC in Linux). */
    volatile uint8_t *region;
    uint64_t region_phys;
    size_t region_bytes;
} ErhaiZapMemory;
typedef struct {
    uint32_t step;              /* 1 header, 2 size, 3 metadata, 4 init_image, 5 mem_setup, 6 segments, 7 auth_and_reset, 8 done */
    int64_t init_image, mem_setup, auth_and_reset;
    uint64_t mem_size, mem_reloc;
    uint32_t metadata_size, segments, relocate;
} ErhaiZapResult;
int64_t ErhaiZapLoad(const uint8_t *image, size_t bytes, uint32_t pas_id, const ErhaiZapIo *, const ErhaiZapMemory *,
                     ErhaiZapResult *);
#ifdef __cplusplus
}
#endif
#endif
