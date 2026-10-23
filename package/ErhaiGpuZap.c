/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ErhaiGpuZap.h"

/* The checks, arithmetic and order of the pinned msm-next mdt_loader.c (mdt_header_valid, qcom_mdt_get_size,
 * qcom_mdt_read_metadata, __qcom_mdt_pas_init, qcom_mdt_bins_are_split, qcom_mdt_load_no_init) and of zap_shader_load_mdt.
 * ELF32 fields keep their 32-bit types, so sums wrap where Linux's do. scripts/check-erhai-gpu-zap.py compares this file
 * with those functions built as a software oracle. */
enum {Einval = 22, E2big = 7, Enomem = 12, Enoent = 2};
enum {EhdrBytes = 52, PhdrBytes = 32, ShdrBytes = 40, PtLoad = 1};
#define MDT_TYPE_MASK (7u << 24)
#define MDT_TYPE_HASH (2u << 24)
#define MDT_RELOCATABLE (1u << 27)

typedef struct {uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;} Phdr;
static uint32_t le32(const uint8_t *p) {return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static Phdr phdr(const uint8_t *image, unsigned i)
{
    const uint8_t *p = image + le32(image + 28) + (size_t)i * PhdrBytes;
    return (Phdr){le32(p), le32(p + 4), le32(p + 8), le32(p + 12), le32(p + 16), le32(p + 20), le32(p + 24), le32(p + 28)};
}
static int header_valid(const uint8_t *image, size_t bytes)
{
    if (bytes < EhdrBytes) return 0;
    if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') return 0;
    if (le16(image + 42) != PhdrBytes) return 0;
    if ((uint64_t)PhdrBytes * le16(image + 44) + le32(image + 28) > bytes) return 0;
    if (le16(image + 46) || le16(image + 48)) {
        if (le16(image + 46) != ShdrBytes) return 0;
        if ((uint64_t)ShdrBytes * le16(image + 48) + le32(image + 32) > bytes) return 0;
    }
    return 1;
}
static int loadable(const Phdr *p) {return p->type == PtLoad && (p->flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && p->memsz;}
/* qcom_mdt_get_size and the range __qcom_mdt_pas_init passes to mem_setup: the 4 KiB-aligned span of the loadable segments. */
static void span(const uint8_t *image, uint64_t *min_addr, uint64_t *max_addr, uint32_t *relocate)
{
    *min_addr = UINT64_MAX; *max_addr = 0; *relocate = 0;
    for (unsigned i = 0; i < le16(image + 44); ++i) {
        const Phdr p = phdr(image, i);
        if (!loadable(&p)) continue;
        if (p.flags & MDT_RELOCATABLE) *relocate = 1;
        if (p.paddr < *min_addr) *min_addr = p.paddr;
        const uint32_t end = p.paddr + p.memsz;
        if (end > *max_addr) *max_addr = (uint32_t)((end + 4095u) & ~4095u);
    }
}
static void copy_out(volatile uint8_t *dst, const uint8_t *src, size_t bytes) {for (size_t i = 0; i < bytes; ++i) dst[i] = src[i];}

int64_t ErhaiZapLoad(const uint8_t *image, size_t bytes, uint32_t pas_id, const ErhaiZapIo *io, const ErhaiZapMemory *memory,
                     ErhaiZapResult *result)
{
    if (!result) return -Einval;
    *result = (ErhaiZapResult){.step = 1};
    if (!image || !io || !io->pas || !io->publish || !memory || !memory->metadata || !memory->metadata_phys || !memory->region ||
        !memory->region_phys || !memory->region_bytes)
        return -Einval;
    if (!header_valid(image, bytes)) return -Einval;
    const unsigned phnum = le16(image + 44);
    /* zap_shader_load_mdt: the image must fit the reserved region. */
    result->step = 2;
    uint64_t min_addr, max_addr;
    uint32_t relocate;
    span(image, &min_addr, &max_addr, &relocate);
    if (!(min_addr < max_addr)) return -Einval;
    const uint64_t mem_size = max_addr - min_addr;
    result->mem_size = mem_size; result->relocate = relocate;
    if (mem_size > memory->region_bytes) return -E2big;
    /* qcom_mdt_read_metadata: the ELF header segment followed by the hash segment, no padding. */
    result->step = 3;
    if (phnum < 2) return -Einval;
    const Phdr first = phdr(image, 0);
    if (first.type == PtLoad) return -Einval;
    unsigned hash_segment = 0;
    for (unsigned i = 1; i < phnum && !hash_segment; ++i)
        if ((phdr(image, i).flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) hash_segment = i;
    if (!hash_segment) return -Einval;
    const size_t ehdr_size = first.filesz, hash_size = phdr(image, hash_segment).filesz;
    size_t hash_offset;
    /* Linux copies p_filesz header bytes unchecked; here a header segment longer than the file is refused. */
    if (ehdr_size > bytes) return -Einval;
    if (ehdr_size + hash_size == bytes) hash_offset = first.filesz;
    else if ((size_t)phdr(image, hash_segment).offset + hash_size <= bytes) hash_offset = phdr(image, hash_segment).offset;
    else return -Enoent;   /* the hash would come from a .bNN file */
    if (ehdr_size + hash_size > memory->metadata_bytes) return -Enomem;
    copy_out(memory->metadata, image, ehdr_size);
    copy_out(memory->metadata + ehdr_size, image + hash_offset, hash_size);
    result->metadata_size = (uint32_t)(ehdr_size + hash_size);
    if (io->publish(io->cookie, memory->metadata, ehdr_size + hash_size)) return -Einval;
    /* qcom_pas_init_image(pas_id, metadata): ARGS(2, VAL, RW). */
    result->step = 4;
    const uint64_t init[2] = {pas_id, memory->metadata_phys};
    if ((result->init_image = io->pas(io->cookie, ErhaiZapPasInitImage, 0x82, init, 2))) return result->init_image;
    if (relocate) {
        result->step = 5;
        const uint64_t setup[3] = {pas_id, memory->region_phys, mem_size};
        if ((result->mem_setup = io->pas(io->cookie, ErhaiZapPasMemSetup, 3, setup, 3))) return result->mem_setup;
    }
    /* qcom_mdt_load_no_init into the first mem_size bytes of the region. */
    result->step = 6;
    int split = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const Phdr p = phdr(image, i);
        if (p.filesz && ((uint64_t)p.offset > bytes || (uint64_t)p.offset + p.filesz > bytes)) split = 1;
    }
    const uint64_t mem_reloc = relocate ? min_addr : memory->region_phys;
    result->mem_reloc = mem_reloc;
    for (unsigned i = 0; i < phnum; ++i) {
        const Phdr p = phdr(image, i);
        if (!loadable(&p)) continue;
        const int64_t offset = (int64_t)((uint64_t)p.paddr - mem_reloc);
        if (offset < 0 || (uint64_t)offset + p.memsz > mem_size) return -Einval;
        if (p.filesz > p.memsz) return -Einval;
        volatile uint8_t *ptr = memory->region + offset;
        if (p.filesz && !split) {
            if ((uint64_t)p.offset + p.filesz > bytes) return -Einval;
            copy_out(ptr, image + p.offset, p.filesz);
        } else if (p.filesz) {
            return -Enoent;
        }
        for (uint32_t z = p.filesz; z < p.memsz; ++z) ptr[z] = 0;
        ++result->segments;
    }
    if (io->publish(io->cookie, memory->region, (size_t)mem_size)) return -Einval;
    /* qcom_pas_auth_and_reset(pas_id): the secure world verifies and starts the zap shader. */
    result->step = 7;
    const uint64_t auth[1] = {pas_id};
    if ((result->auth_and_reset = io->pas(io->cookie, ErhaiZapPasAuthAndReset, 1, auth, 1))) return result->auth_and_reset;
    result->step = 8;
    return 0;
}
