/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* ErhaiGpuHwInit with the gen8 NativeAdreno ring and CpInit, in mode 9's order, against the pinned Linux a8xx hw_init built
 * as an oracle (scripts/check-erhai-gpu-hwinit.py). Both run over the same register model; every register access, the
 * OOB/SMMU/flush/idle/zap events, the ring words and the pwrup page must agree. Software peers only. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoCpInit.h"
#include "ErhaiGpuHwInit.h"
#include "ErhaiGpuSource.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" {
int oracle_hw_init(uint64_t sqe_iova, uint64_t ring_iova, uint64_t memptrs_iova, uint32_t *ring, unsigned ring_words, void *pwrup,
                   uint64_t pwrup_iova);
void oracle_access(int write, uint32_t reg, uint32_t value);
uint32_t oracle_read(uint32_t reg);
void oracle_ring(uint32_t word);
int oracle_event(int id);
}
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1);}} while (0)
enum {EventOobSet = 1, EventAperture, EventFlush, EventIdle, EventZap};
constexpr uint64_t kSqe = 0x40000000, kRing = 0x40100000, kMemptrs = 0x40108000, kPwrup = 0x40110000;
constexpr uint32_t kScratch = 0x082c;

struct Access {
    char kind;   /* W write, R read, E event */
    uint32_t reg, value;
    bool operator==(const Access &o) const {return kind == o.kind && reg == o.reg && value == o.value;}
};
struct Model {
    std::map<uint32_t, uint32_t> regs;
    std::vector<Access> log;
    std::vector<uint32_t> ring;
    std::map<uint32_t, uint32_t> last;   /* the last value written to each register, filtered or not */
    uint32_t p2s = ERHAI_GPU_RBBM_CGC_P2S_STATUS_TXDONE;
    size_t writes = 0, fail_write = 0;   /* 1-based index of the write that fails, 0 for none */
    unsigned waits = 0;
};
static Model m;
static uint32_t seeded(uint32_t reg) {return reg * 0x9e3779b1u ^ 0x5a5a1234u;}
static bool ring_register(uint32_t reg)
{
    return reg == ERHAI_GPU_CP_RB_BASE || reg == ERHAI_GPU_CP_RB_BASE + 1 || reg == ERHAI_GPU_CP_RB_CNTL ||
           reg == ERHAI_GPU_CP_RB_RPTR_ADDR || reg == ERHAI_GPU_CP_RB_RPTR_ADDR + 1 || reg == ERHAI_GPU_CP_RB_WPTR ||
           reg == ERHAI_GPU_CP_RB_RPTR || reg == kScratch;
}
static int model_write(uint32_t reg, uint32_t value)
{
    if (++m.writes == m.fail_write) return 1;
    m.regs[reg] = value; m.last[reg] = value;
    if (!ring_register(reg)) m.log.push_back({'W', reg, value});
    return 0;
}
static uint32_t model_read(uint32_t reg)
{
    /* An idle CP: RPTR stays 0, and P2S_STATUS is the test's choice; any other register reads back or its seed. */
    const uint32_t value = reg == ERHAI_GPU_RBBM_CGC_P2S_STATUS ? m.p2s : reg == ERHAI_GPU_CP_RB_RPTR ? 0 :
                           m.regs.count(reg) ? m.regs[reg] : seeded(reg);
    /* The ring's RPTR reads and CpInit's read-back of the SQE base Linux never does are not part of the comparison. */
    if (!ring_register(reg) && reg != ERHAI_GPU_CP_SQE_INSTR_BASE && reg != ERHAI_GPU_CP_SQE_INSTR_BASE + 1)
        m.log.push_back({'R', reg, value});
    return value;
}
extern "C" void oracle_access(int, uint32_t reg, uint32_t value) {CHECK(!model_write(reg, value));}
extern "C" uint32_t oracle_read(uint32_t reg) {return model_read(reg);}
extern "C" void oracle_ring(uint32_t word) {m.ring.push_back(word);}
extern "C" int oracle_event(int id) {m.log.push_back({'E', (uint32_t)id, 0}); return 0;}

/* The board side's platform. */
static uint32_t ringmem[0x8000 / 4], fencemem[1024], pwrup[2048], oracle_ringmem[0x8000 / 4], oracle_pwrup[2048];
static int hw_write(void *c, uint32_t reg, uint32_t value)
{
    CHECK(c == &m);
    if (model_write(reg, value)) return 1;
    /* A WPTR advance is the ring's flush; attach only zeroes it. */
    if (reg == ERHAI_GPU_CP_RB_WPTR && value) m.log.push_back({'E', EventFlush, 0});
    return 0;
}
static int hw_read(void *c, uint32_t reg, uint32_t *value) {CHECK(c == &m && value); *value = model_read(reg); return 0;}
static int hw_wait(void *c, unsigned us) {CHECK(c == &m && us == 1); ++m.waits; return 0;}
static int hw_aperture(void *c) {CHECK(c == &m); m.log.push_back({'E', EventAperture, 0}); return 0;}
static int barrier(void *c) {CHECK(c == &m); return 0;}
static int publish(void *c, const volatile uint32_t *, size_t) {CHECK(c == &m); return 0;}
static int acquire(void *c, volatile uint32_t *, size_t) {CHECK(c == &m); return 0;}
static uint64_t now_us(void *) {static uint64_t t; return t += 10;}
static const ErhaiGpuIo io{&m, hw_write, hw_read, hw_wait, hw_aperture};
static const NativeAdrenoRingIo ring_io{&m, hw_write, hw_read, barrier};
static const NativeAdrenoRingCache cache{&m, publish, acquire};
static const NativeAdrenoRingClock clock_{&m, now_us, nullptr};
static const ErhaiGpuHwConfig config{kSqe, kMemptrs + 8};

static void reset(uint32_t p2s)
{
    m = Model{}; m.p2s = p2s;
    std::memset(ringmem, 0xee, sizeof ringmem); std::memset(pwrup, 0, sizeof pwrup);
}
static void submit_and_wait(NativeAdrenoRing &ring, uint32_t fence)
{
    fencemem[1] = fence;
    CHECK(ring.wait(fence, &clock_, 1000, nullptr) == NativeAdrenoRingFenceComplete);
    m.log.push_back({'E', EventIdle, 0});
}
/* mode 9: OOB GPU_SET (mode 8), Pre, ring attach, StartCp, CP init, zap, CP_SET_SECURE_MODE 0, Post. */
static ErhaiGpuHw board(size_t *written)
{
    m.log.push_back({'E', EventOobSet, 0});
    ErhaiGpuHw hw{};
    CHECK(ErhaiGpuHwInitPre(&hw, &io, &config) == ErhaiGpuOk && hw.step == 7 && hw.smmu_aperture == 0);
    NativeAdrenoRing ring;
    CHECK(ring.attach(&ring_io, &cache, ringmem, sizeof ringmem, kRing, fencemem, sizeof fencemem, kMemptrs) == kIOReturnSuccess);
    CHECK(ErhaiGpuHwInitStartCp(&hw, &io, &config) == ErhaiGpuOk);
    NativeAdrenoCpInit init;
    const NativeAdrenoCpInitConfig cp{kSqe, kPwrup, 1};
    uint32_t fence = 0;
    CHECK(init.run(&ring_io, &ring, &cp, &fence) == kIOReturnSuccess && fence == 1);
    submit_and_wait(ring, fence);
    m.log.push_back({'E', EventZap, 0});
    uint32_t secure[2];
    CHECK(!ErhaiGpuSecureModePacket(secure));
    CHECK(ring.submit(secure, 2, &fence) == kIOReturnSuccess && fence == 2);
    submit_and_wait(ring, fence);
    CHECK(ErhaiGpuHwInitPost(&hw, &io, pwrup, sizeof pwrup, written) == ErhaiGpuOk && hw.step == 13);
    CHECK(ring.detach() == kIOReturnSuccess);
    return hw;
}
static void compare(uint32_t p2s, bool txdone)
{
    reset(p2s); std::memset(oracle_pwrup, 0, sizeof oracle_pwrup);
    CHECK(oracle_hw_init(kSqe, kRing, kMemptrs, oracle_ringmem, sizeof oracle_ringmem / 4, oracle_pwrup, kPwrup) == 0);
    const Model linux_run = m;
    size_t written = 0;
    reset(p2s);
    const ErhaiGpuHw hw = board(&written);
    size_t n = 0;
    while (n < linux_run.log.size() && n < m.log.size() && linux_run.log[n] == m.log[n]) ++n;
    if (n != linux_run.log.size() || n != m.log.size()) {
        std::fprintf(stderr, "access %zu of %zu/%zu differs:", n, linux_run.log.size(), m.log.size());
        if (n < linux_run.log.size()) std::fprintf(stderr, " linux %c %#x %#x", linux_run.log[n].kind, linux_run.log[n].reg, linux_run.log[n].value);
        if (n < m.log.size()) std::fprintf(stderr, " board %c %#x %#x", m.log[n].kind, m.log[n].reg, m.log[n].value);
        std::fprintf(stderr, "\n"); std::exit(1);
    }
    /* The ring: Linux's base and size; the RPTR shadow is the fence page's +0 instead of a separate shadow buffer. */
    CHECK(linux_run.last.at(ERHAI_GPU_CP_RB_BASE) == m.last.at(ERHAI_GPU_CP_RB_BASE) &&
          linux_run.last.at(ERHAI_GPU_CP_RB_BASE + 1) == m.last.at(ERHAI_GPU_CP_RB_BASE + 1));
    CHECK(linux_run.last.at(ERHAI_GPU_CP_RB_CNTL) == m.last.at(ERHAI_GPU_CP_RB_CNTL) &&
          m.last.at(ERHAI_GPU_CP_RB_CNTL) == ERHAI_GPU_RB_CNTL_DEFAULT);
    CHECK(m.last.at(ERHAI_GPU_CP_RB_RPTR_ADDR) == (uint32_t)kMemptrs && linux_run.last.at(ERHAI_GPU_CP_RB_RPTR_ADDR) == (uint32_t)kMemptrs);
    /* Ring words: CP_THREAD_CONTROL + CP_ME_INIT (10), the ring fence (7), CP_SET_SECURE_MODE 0 (2), the fence (7). */
    CHECK(linux_run.ring.size() == 12);
    CHECK(!std::memcmp(ringmem, linux_run.ring.data(), 10 * 4) && !std::memcmp(ringmem + 17, linux_run.ring.data() + 10, 2 * 4));
    /* The power-up page byte for byte, and nothing past what the board wrote. */
    CHECK(written > 16 && written < sizeof pwrup && !std::memcmp(pwrup, oracle_pwrup, sizeof pwrup));
    CHECK(hw.pwrup_ifpc == ERHAI_GPU_IFPC_COUNT && hw.pwrup_preemption == ERHAI_GPU_PWRUP_COUNT && hw.pwrup_dynamic > 0);
    CHECK(written == 16 + (hw.pwrup_ifpc + hw.pwrup_preemption) * 8 + hw.pwrup_dynamic * 12);
    CHECK(hw.gmem_protect && txdone == !!(hw.p2s_status & ERHAI_GPU_RBBM_CGC_P2S_STATUS_TXDONE) && m.waits == (txdone ? 0u : 11u));
}
int main()
{
    compare(ERHAI_GPU_RBBM_CGC_P2S_STATUS_TXDONE, true);
    const size_t accesses = m.log.size();
    compare(0, false);   /* TXDONE never seen: no CLOCK_CNTL_GLOBAL = 0, the OOB clear still happens */
    /* A failed write at every Pre position stops at once and names the register. */
    unsigned failures = 0;
    for (size_t k = 1;; ++k) {
        reset(ERHAI_GPU_RBBM_CGC_P2S_STATUS_TXDONE); m.fail_write = k;
        ErhaiGpuHw hw{};
        const ErhaiGpuStatus s = ErhaiGpuHwInitPre(&hw, &io, &config);
        if (s == ErhaiGpuOk) {CHECK(m.writes == k - 1); break;}
        CHECK(s == ErhaiGpuIoError && m.writes == k && hw.failed_reg && hw.step >= 1 && hw.step <= 6);
        ++failures;
    }
    /* Order and argument refusals precede any register access. */
    {
        reset(0); ErhaiGpuHw hw{}; size_t written = 9;
        const ErhaiGpuHwConfig unaligned{kSqe + 4, kMemptrs + 8}, zero{0, kMemptrs + 8}, no_bv{kSqe, 0};
        const ErhaiGpuIo no_wait{&m, hw_write, hw_read, nullptr, hw_aperture};
        CHECK(ErhaiGpuHwInitPre(nullptr, &io, &config) == ErhaiGpuArgument);
        CHECK(ErhaiGpuHwInitPre(&hw, nullptr, &config) == ErhaiGpuArgument);
        CHECK(ErhaiGpuHwInitPre(&hw, &no_wait, &config) == ErhaiGpuArgument);
        CHECK(ErhaiGpuHwInitPre(&hw, &io, &unaligned) == ErhaiGpuArgument);
        CHECK(ErhaiGpuHwInitPre(&hw, &io, &zero) == ErhaiGpuArgument);
        CHECK(ErhaiGpuHwInitStartCp(&hw, &io, &config) == ErhaiGpuArgument);
        CHECK(ErhaiGpuHwInitPost(&hw, &io, pwrup, sizeof pwrup, &written) == ErhaiGpuArgument);
        CHECK(m.writes == 0 && m.log.empty());
        CHECK(ErhaiGpuHwInitPre(&hw, &io, &config) == ErhaiGpuOk);
        CHECK(ErhaiGpuHwInitPost(&hw, &io, pwrup, sizeof pwrup, &written) == ErhaiGpuArgument);
        const size_t before = m.writes;
        CHECK(ErhaiGpuHwInitStartCp(&hw, &io, &no_bv) == ErhaiGpuArgument && m.writes == before);
        CHECK(ErhaiGpuHwInitStartCp(&hw, &io, &config) == ErhaiGpuOk);
        /* A page too small for the lists: GMEM_PROTECT is written, then the list is refused before any read. */
        const size_t reads = m.log.size();
        CHECK(ErhaiGpuHwInitPost(&hw, &io, pwrup, 64, &written) == ErhaiGpuArgument && written == 0 && hw.step == 10);
        for (size_t i = reads; i < m.log.size(); ++i) CHECK(m.log[i].kind == 'W');
    }
    std::printf("PASS: erhai A830 hw_init equals the pinned Linux a8xx hw_init (%zu register accesses and events, ring words, "
                "pwrup page; with and without P2S TXDONE), %u Pre write failures, 10 order/argument refusals\n", accesses, failures);
}
