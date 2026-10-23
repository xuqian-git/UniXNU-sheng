/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The gen8 NativeAdreno framed submission, interrupt acknowledgement and always-on counter against the pinned Linux
 * a7xx_submit / a6xx_set_pagetable / a8xx_irq built as an oracle (scripts/check-erhai-gpu-submit.py): the registers both
 * command streams name, the CP waits and the indirect buffer packet, the submission tail word for word, the interrupt's
 * register accesses and the A8XX offsets and masks. Software peers only. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoTimer.h"
#include "AdrenoPM4.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
void oracle_submit(uint64_t memptrs_iova, uint32_t *ring, unsigned ring_words, uint64_t ib_iova, uint32_t ib_dwords, uint32_t seqno,
                   int new_context);
void oracle_irq(void);
extern const uint32_t oracle_cp_always_on, oracle_gmu_always_on_l, oracle_scratch, oracle_int_mask, oracle_cp_global_int_mask;
void oracle_ring(uint32_t word);
void oracle_access(int write, uint32_t reg, uint32_t value);
uint32_t oracle_read(uint32_t reg);
void oracle_event(int id);
}
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1);}} while (0)
enum {OracleFlush = 1, OracleRetire, OraclePreempt, OracleHang, OracleCpHw, OracleFuse};
enum : uint32_t {OpWaitTimestamp = 0x14, OpResetContextState = 0x1f, OpWaitRegMem = 0x3c, OpRegToMem = 0x3e, OpIndirectBuffer = 0x3f};
constexpr uint64_t kRing = 0x40100000, kFence = 0x40108000, kIb = 0x50000000;
constexpr uint32_t kGmuWindow = 0x6c000, kIntStatus = 0x6a, kIntClear = 0x61;

struct Access {
    char kind;   /* W write, R read, E event */
    uint32_t reg, value;
    bool operator==(const Access &o) const {return kind == o.kind && reg == o.reg && value == o.value;}
};
static std::vector<Access> accesses;
static std::vector<uint32_t> oracle_words;
static uint32_t status_value;
void oracle_ring(uint32_t word) {oracle_words.push_back(word);}
void oracle_access(int write, uint32_t reg, uint32_t value) {accesses.push_back({write ? 'W' : 'R', reg, value});}
uint32_t oracle_read(uint32_t reg)
{
    const uint32_t value = reg == kIntStatus ? status_value : 0;
    accesses.push_back({'R', reg, value});
    return value;
}
void oracle_event(int id) {accesses.push_back({'E', (uint32_t)id, 0});}

static int board_write(void *, uint32_t reg, uint32_t value) {accesses.push_back({'W', reg, value}); return 0;}
static int board_read(void *, uint32_t reg, uint32_t *value) {*value = reg == kIntStatus ? status_value : 0; accesses.push_back({'R', reg, *value}); return 0;}
static int board_barrier(void *) {return 0;}
static int publish(void *, const volatile uint32_t *, size_t) {return 0;}
static int acquire(void *, volatile uint32_t *, size_t) {return 0;}
static const NativeAdrenoRingIo kIo = {nullptr, board_write, board_read, board_barrier};
static const NativeAdrenoRingCache kCache = {nullptr, publish, acquire};

/* The packets that name a register, a wait or the command buffer, with their payloads: TYPE4 writes, CP_WAIT_REG_MEM,
 * CP_WAIT_TIMESTAMP, CP_RESET_CONTEXT_STATE and CP_INDIRECT_BUFFER. The statistics CP_REG_TO_MEM reads are counted apart. */
static std::vector<uint32_t> named(const std::vector<uint32_t> &words, unsigned *stats)
{
    AdrenoPM4Packet packets[128];
    size_t count = 0;
    CHECK(AdrenoPM4Plan(words.data(), words.size(), packets, 128, &count) == AdrenoPM4Success);
    std::vector<uint32_t> out;
    if (stats) *stats = 0;
    for (size_t i = 0; i < count; ++i) {
        const AdrenoPM4Packet &p = packets[i];
        const bool keep = p.type == 4 || p.identifier == OpWaitRegMem || p.identifier == OpWaitTimestamp ||
                          p.identifier == OpResetContextState || p.identifier == OpIndirectBuffer;
        if (p.type == 7 && p.identifier == OpRegToMem && stats) ++*stats;
        if (!keep) continue;
        out.push_back(p.type << 24 | p.identifier);
        for (uint32_t w = 0; w <= p.payload_words; ++w) out.push_back(words[p.header_word + w]);
    }
    return out;
}
/* From the CP_SCRATCH_GLOBAL(2) write to the end. */
static std::vector<uint32_t> tail(const std::vector<uint32_t> &words)
{
    uint32_t header = 0;
    CHECK(AdrenoPM4Type4(oracle_scratch, 1, &header) == AdrenoPM4Success);
    for (size_t i = 0; i < words.size(); ++i)
        if (words[i] == header) return std::vector<uint32_t>(words.begin() + (long)i, words.end());
    CHECK(!"no scratch write");
    return {};
}

int main()
{
    unsigned compared = 0;
    /* A8XX offsets and masks. */
    CHECK(NativeAdrenoTimer::cpLowRegister() == oracle_cp_always_on && NativeAdrenoTimer::cpHighRegister() == oracle_cp_always_on + 1);
    CHECK(NativeAdrenoTimer::gmuLowOffset() == oracle_gmu_always_on_l * 4 - kGmuWindow &&
          NativeAdrenoTimer::gmuHighOffset() == NativeAdrenoTimer::gmuLowOffset() + 4);
    CHECK(NativeAdrenoIrq::mask() == oracle_int_mask && NativeAdrenoIrq::cpErrorMask() == oracle_cp_global_int_mask);

    /* Two submissions: the first of a new context (a6xx_set_pagetable's entry), the second in the same context. */
    static uint32_t ring_memory[2048], fence_memory[64], linux_ring[2048];
    NativeAdrenoRing ring;
    CHECK(ring.attach(&kIo, &kCache, ring_memory, sizeof ring_memory, kRing, fence_memory, sizeof fence_memory, kFence) == kIOReturnSuccess);
    uint32_t start = 0;
    for (uint32_t seqno = 1; seqno <= 2; ++seqno) {
        const uint32_t ib_dwords = 4 + seqno;
        uint32_t ib[4];
        CHECK(AdrenoPM4Type7(OpIndirectBuffer, 3, &ib[0]) == AdrenoPM4Success);
        ib[1] = (uint32_t)kIb; ib[2] = (uint32_t)(kIb >> 32); ib[3] = ib_dwords;
        if (seqno == 1) ring.requestContextReset();
        accesses.clear();
        uint32_t fence = 0;
        CHECK(ring.submitFramed(ib, 4, &fence) == kIOReturnSuccess && fence == seqno);
        CHECK(!accesses.empty() && accesses.back().kind == 'W');   /* only the RPTR read and the WPTR write */
        const uint32_t end = accesses.back().value;
        std::vector<uint32_t> board(ring_memory + start, ring_memory + end);
        start = end;
        oracle_words.clear(); accesses.clear();
        oracle_submit(kFence, linux_ring, 2048, kIb, ib_dwords, seqno, seqno == 1);
        CHECK(accesses.size() == 2 && accesses[0].kind == 'E' && accesses[0].reg == OracleFlush && accesses[1].reg == OraclePreempt);
        unsigned stats = 0;
        const std::vector<uint32_t> linux_named = named(oracle_words, &stats), board_named = named(board, nullptr);
        if (linux_named != board_named) {
            std::fprintf(stderr, "submission %u: named packets differ\n", seqno);
            for (size_t i = 0; i < std::max(linux_named.size(), board_named.size()); ++i)
                std::fprintf(stderr, "  %3zu linux %08x board %08x\n", i, i < linux_named.size() ? linux_named[i] : 0,
                             i < board_named.size() ? board_named[i] : 0);
            std::exit(1);
        }
        CHECK(stats == 4);
        CHECK(tail(oracle_words) == tail(board) && tail(board).size() == 31);
        compared += (unsigned)(linux_named.size() + tail(board).size());
    }

    /* Interrupt acknowledgement: the register accesses for each status without a Linux-only error read. */
    const uint32_t statuses[] = {1u << 20, (1u << 20) | (1u << 17), 0, 1u << 23, 1u << 8, (1u << 20) | (1u << 29)};
    NativeAdrenoIrq irq;
    CHECK(irq.attach(&kIo) == kIOReturnSuccess);
    for (uint32_t status : statuses) {
        status_value = status;
        accesses.clear();
        oracle_irq();
        std::vector<Access> linux_log;
        for (const Access &a : accesses) if (a.kind != 'E') linux_log.push_back(a);
        const bool retire = std::find(accesses.begin(), accesses.end(), Access{'E', OracleRetire, 0}) != accesses.end();
        accesses.clear();
        NativeAdrenoIrqResult result;
        CHECK(irq.handle(&result) == kIOReturnSuccess && result.status == status);
        CHECK(accesses == linux_log && linux_log.size() == 2 && linux_log[1] == (Access{'W', kIntClear, status}));
        CHECK(retire == !!(result.flags & NativeAdrenoIrqFence));
        ++compared;
    }
    std::printf("PASS: gen8 NativeAdrenoRing framed submission (new and same context), NativeAdrenoIrq and NativeAdrenoTimer equal the "
                "pinned Linux a7xx_submit/a6xx_set_pagetable/a8xx_irq: %u compared words/cases\n", compared);
    return 0;
}
