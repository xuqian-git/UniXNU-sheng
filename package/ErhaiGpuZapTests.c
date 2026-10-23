/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* ErhaiZapLoad against the pinned Linux mdt_loader.c / zap_shader_load_mdt oracle (scripts/check-erhai-gpu-zap.py), on the
 * local gen80000_zap.mbn and variants of it: results, PAS calls, metadata and region bytes must agree. Software peers only. */
#include "ErhaiGpuZap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long oracle_zap(const uint8_t *data, size_t size, int pasid, void *region, uint64_t mem_phys, size_t region_size);
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1);}} while (0)
enum {Pas = 13, RegionBytes = 0x2000};
static const uint64_t kRegionPhys = 0x9b09a000, kMetadataPhys = 0x80000000;

typedef struct {uint32_t cmd, count; uint64_t args[3];} Call;
typedef struct {
    Call calls[8];
    unsigned ncalls, publishes;
    uint8_t metadata[8192];
    size_t metadata_len;
    int64_t fail_cmd_result[6];   /* the result a PAS command returns, by command */
} Log;
static Log log_;
static uint8_t region[RegionBytes], metadata_buffer[8192];

static int64_t record(uint32_t cmd, const uint64_t *args, unsigned count)
{
    CHECK(log_.ncalls < 8 && count <= 3 && cmd < 6);
    Call *c = &log_.calls[log_.ncalls++];
    c->cmd = cmd; c->count = count;
    for (unsigned i = 0; i < count; ++i) c->args[i] = args[i];
    return log_.fail_cmd_result[cmd];
}
long oracle_pas(uint32_t cmd, const uint64_t *args, unsigned count, const void *metadata, size_t metadata_len)
{
    if (metadata) {CHECK(metadata_len <= sizeof log_.metadata); memcpy(log_.metadata, metadata, metadata_len); log_.metadata_len = metadata_len;}
    return (long)record(cmd, args, count);
}
static int64_t board_pas(void *c, uint32_t cmd, uint32_t arginfo, const uint64_t *args, unsigned count)
{
    CHECK(c == &log_);
    CHECK((cmd == 1 && arginfo == 0x82 && count == 2 && args[1] == kMetadataPhys) || (cmd == 2 && arginfo == 3 && count == 3) ||
          (cmd == 5 && arginfo == 1 && count == 1));
    if (cmd == 1) {memcpy(log_.metadata, metadata_buffer, sizeof metadata_buffer); log_.metadata_len = 0;}
    uint64_t a[3] = {args[0], cmd == 1 ? 0 : count > 1 ? args[1] : 0, count > 2 ? args[2] : 0};
    return record(cmd, a, count);
}
static int publish(void *c, const volatile void *p, size_t bytes)
{
    CHECK(c == &log_ && p && bytes);
    ++log_.publishes; return 0;
}
static const ErhaiZapIo io = {&log_, board_pas, publish};

typedef struct {long ret; Log log; uint8_t region[RegionBytes];} Run;
static void prepare(const int64_t *results) {memset(&log_, 0, sizeof log_); if (results) memcpy(log_.fail_cmd_result, results, sizeof log_.fail_cmd_result); memset(region, 0xa5, sizeof region); memset(metadata_buffer, 0, sizeof metadata_buffer);}
static unsigned compare(const char *name, const uint8_t *image, size_t bytes, const int64_t *results, long expected)
{
    Run linux_run, board;
    prepare(results);
    linux_run.ret = oracle_zap(image, bytes, Pas, region, kRegionPhys, RegionBytes);
    linux_run.log = log_; memcpy(linux_run.region, region, sizeof region);
    prepare(results);
    ErhaiZapResult r;
    const ErhaiZapMemory memory = {metadata_buffer, kMetadataPhys, sizeof metadata_buffer, region, kRegionPhys, RegionBytes};
    board.ret = (long)ErhaiZapLoad(image, bytes, Pas, &io, &memory, &r);
    board.log = log_; memcpy(board.region, region, sizeof region);
    if (board.ret != linux_run.ret || board.ret != expected) {
        fprintf(stderr, "%s: linux %ld board %ld (step %u) expected %ld\n", name, linux_run.ret, board.ret, r.step, expected); exit(1);
    }
    CHECK(board.log.ncalls == linux_run.log.ncalls);
    for (unsigned i = 0; i < board.log.ncalls; ++i)
        CHECK(!memcmp(&board.log.calls[i], &linux_run.log.calls[i], sizeof(Call)));
    if (linux_run.log.metadata_len) {
        CHECK(r.metadata_size == linux_run.log.metadata_len && !memcmp(board.log.metadata, linux_run.log.metadata, r.metadata_size));
    }
    CHECK(!memcmp(board.region, linux_run.region, sizeof region));
    return 1;
}
static void put32(uint8_t *image, size_t at, uint32_t v) {for (int i = 0; i < 4; ++i) image[at + i] = (uint8_t)(v >> (8 * i));}
static uint32_t get32(const uint8_t *image, size_t at) {return image[at] | image[at + 1] << 8 | image[at + 2] << 16 | (uint32_t)image[at + 3] << 24;}
static size_t ph(const uint8_t *image, unsigned i, unsigned field) {return get32(image, 28) + 32u * i + 4u * field;}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    CHECK(f);
    static uint8_t zap[65536], v[65536];
    const size_t bytes = fread(zap, 1, sizeof zap, f);
    fclose(f);
    CHECK(bytes > 52 && bytes < sizeof zap);
    unsigned cases = 0;
    /* The real image: init_image, mem_setup (relocatable), one segment, auth_and_reset. */
    cases += compare("zap", zap, bytes, NULL, 0);
    CHECK(log_.ncalls == 3 && log_.calls[1].cmd == 2 && log_.calls[1].args[1] == kRegionPhys && log_.publishes == 2);
    /* Secure-world refusals stop where Linux stops. */
    const int64_t init_fail[6] = {0, -22}, setup_fail[6] = {0, 0, -5}, auth_fail[6] = {0, 0, 0, 0, 0, -95};
    cases += compare("init_image refused", zap, bytes, init_fail, -22);
    cases += compare("mem_setup refused", zap, bytes, setup_fail, -5);
    cases += compare("auth_and_reset refused", zap, bytes, auth_fail, -95);
    /* Variants of the image. */
    memcpy(v, zap, bytes); v[1] = 'X';
    cases += compare("bad magic", v, bytes, NULL, -22);
    cases += compare("truncated header", zap, 40, NULL, -22);
    memcpy(v, zap, bytes); put32(v, ph(v, 1, 6), get32(v, ph(v, 1, 6)) & ~(1u << 27));
    cases += compare("not relocatable", v, bytes, NULL, -22);       /* p_paddr below the region: segment outside memory */
    memcpy(v, zap, bytes); put32(v, ph(v, 1, 4), get32(v, ph(v, 1, 5)) + 4);
    cases += compare("filesz > memsz", v, bytes, NULL, -22);
    memcpy(v, zap, bytes); put32(v, ph(v, 1, 5), 0x3000);
    cases += compare("larger than the region", v, bytes, NULL, -7);
    memcpy(v, zap, bytes); put32(v, ph(v, 1, 5), 0x1800);
    cases += compare("zeroed tail", v, bytes, NULL, 0);
    memcpy(v, zap, bytes); put32(v, ph(v, 0, 0), 1);
    cases += compare("header segment loadable", v, bytes, NULL, -22);
    memcpy(v, zap, bytes); put32(v, ph(v, 2, 6), 0);
    cases += compare("no hash segment", v, bytes, NULL, -22);
    memcpy(v, zap, bytes); put32(v, ph(v, 1, 1), (uint32_t)bytes);
    cases += compare("segment beyond the file", v, bytes, NULL, -2);  /* split: the .b01 file does not exist */
    /* A packed .mdt (header segment then hash, nothing else): metadata from the file, the segment would be a .b01 file. */
    {
        const size_t ehdr = get32(zap, ph(zap, 0, 4)), hash = get32(zap, ph(zap, 2, 4));
        memcpy(v, zap, ehdr); memcpy(v + ehdr, zap + get32(zap, ph(zap, 2, 1)), hash);
        cases += compare("packed mdt", v, ehdr + hash, NULL, -2);
    }
    /* Board-only refusals: nothing reaches the secure world. */
    {
        ErhaiZapResult r;
        prepare(NULL);
        ErhaiZapMemory small = {metadata_buffer, kMetadataPhys, 64, region, kRegionPhys, RegionBytes};
        CHECK(ErhaiZapLoad(zap, bytes, Pas, &io, &small, &r) == -12 && r.step == 3 && log_.ncalls == 0);
        ErhaiZapMemory none = {0};
        CHECK(ErhaiZapLoad(zap, bytes, Pas, &io, &none, &r) == -22 && log_.ncalls == 0);
        CHECK(ErhaiZapLoad(zap, bytes, Pas, &io, NULL, &r) == -22 && ErhaiZapLoad(zap, bytes, Pas, &io, &small, NULL) == -22);
        memcpy(v, zap, bytes); put32(v, ph(v, 0, 4), (uint32_t)bytes + 1);
        CHECK(ErhaiZapLoad(v, bytes, Pas, &io, &small, &r) == -22 && log_.ncalls == 0);
        cases += 5;
    }
    printf("PASS: erhai zap load equals the pinned Linux mdt_loader/zap_shader_load_mdt on gen80000_zap.mbn (%zu bytes) "
           "and variants, %u cases\n", bytes, cases);
    return 0;
}
