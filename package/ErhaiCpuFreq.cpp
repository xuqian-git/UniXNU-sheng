// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off CPU frequency control for erhai (SM8750). Unlike SM8550's EPSS registers (CpuFreqProbe.cpp), SM8750's CPU DVFS is
// owned by the CPUCP firmware and reached through SCMI's performance protocol (0x13). Layout from the device's live DTB:
//   shared memory  /soc/mmio-sram@0x17b4e000/scmi-shmem@0 (a2p, 0x200 bytes)
//   doorbell       /soc/qcom,cpucp@0x17830000 "tx" window, channel 0 (qcom-cpucp-mbox: write 0 to 0x100 + 8*chan + 4)
//   domains        cpus 0-5 clocks = <&scmi_perf 0>, cpus 6-7 <&scmi_perf 1>
//   cycle counters /soc/walt/qcom,cycle-cntr "freq-domain0/1" at 0x18880400 / 0x19880400 (64-bit, one per domain)
// The shared memory follows arm,scmi-shmem (channel status +0x4, flags +0x10, length +0x14, header +0x18, payload +0x1c); the
// agent polls the channel-free bit instead of taking the completion interrupt. `erhai-cpufreq=<n>`:
//   1  read-only: describe each domain's levels and measure each domain's clock from its cycle counter
//   2  additionally request each domain's highest level (at or below `erhai-cpufreq-cap=<MHz>` when set)
// `erhai-cpufreq-cnt=0` skips the cycle counters.
// The status line goes to native_cpufreq_row (the CPU banner row); CpuFreqProbe must stay off (sheng-cpufreq unset).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern "C" {
extern char native_cpufreq_row[120];
extern volatile uint32_t native_cpufreq_stage;
}

namespace {
constexpr uint64_t kShmemBase = 0x17b4e000, kShmemSize = 0x200;
constexpr uint64_t kDoorbellBase = 0x17830000, kDoorbellSize = 0x300;
constexpr uint32_t kDoorbellChan0 = 0x100 + 4;
constexpr uint64_t kCycleBase[2] = {0x18880400, 0x19880400};
constexpr uint32_t kShmStatus = 0x4, kShmFlags = 0x10, kShmLength = 0x14, kShmHeader = 0x18, kShmPayload = 0x1c;
constexpr uint32_t kChannelFree = 1, kChannelError = 2;
constexpr uint32_t kPerf = 0x13;
constexpr uint32_t kMsgVersion = 0x0, kMsgDomainAttributes = 0x3, kMsgDescribeLevels = 0x4,
                   kMsgLimitsSet = 0x5, kMsgLevelSet = 0x7, kMsgLevelGet = 0x8;
constexpr unsigned kDomains = 2;

struct Domain {
    uint32_t flags, sustainedKHz, levels, minLevel, maxLevel, picked, pickedIndex, setValue;
    int setStatus, limitStatus;
    uint32_t level;     // LEVEL_GET after the vote
    uint32_t mhz;       // measured from the cycle counter
};
IOMemoryMap *gShmMap, *gBellMap, *gCycleMap[kDomains];
uintptr_t gShm, gBell, gCycle[kDomains];
uint32_t gVersion, gToken;
int gError;
Domain gDomain[kDomains];
unsigned gMode;
uint32_t gCapKHz;

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(base + reg) = value;}
uint64_t rd64(uintptr_t base) {return *reinterpret_cast<volatile uint64_t *>(base);}

uintptr_t mapRange(uint64_t base, uint64_t size, IOMemoryMap **out) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    *out = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    return *out ? static_cast<uintptr_t>((*out)->getVirtualAddress()) : 0;
}

uint64_t nowUs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}

// One polled SCMI transaction. Returns the SCMI status (0 success) or a negative transport error; the response payload after the
// status word is copied to `out` (up to outWords words).
int scmiCall(uint32_t message, const uint32_t *in, unsigned inWords, uint32_t *out, unsigned outWords, unsigned *gotWords) {
    uint64_t start = nowUs();
    while (!(rd32(gShm, kShmStatus) & kChannelFree)) {
        if (nowUs() - start > 100000) return -100;
        IODelay(10);
    }
    const uint32_t token = (gToken++) & 0x3ff;
    wr32(gShm, kShmStatus, 0);
    wr32(gShm, kShmFlags, 0);
    wr32(gShm, kShmLength, 4 + inWords * 4);
    wr32(gShm, kShmHeader, (message & 0xff) | (kPerf << 10) | (token << 18));
    for (unsigned i = 0; i < inWords; ++i) wr32(gShm, kShmPayload + i * 4, in[i]);
    __asm__ volatile("dsb sy" ::: "memory");
    wr32(gBell, kDoorbellChan0, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    start = nowUs();
    uint32_t status;
    while (!((status = rd32(gShm, kShmStatus)) & kChannelFree)) {
        if (nowUs() - start > 100000) return -101;
        IODelay(10);
    }
    if (status & kChannelError) return -102;
    const uint32_t length = rd32(gShm, kShmLength);
    if (length < 8 || length > kShmemSize - kShmHeader) return -103;
    const unsigned words = (length - 8) / 4;
    const int scmiStatus = (int)rd32(gShm, kShmPayload);
    unsigned copied = 0;
    for (; copied < words && copied < outWords; ++copied) out[copied] = rd32(gShm, kShmPayload + 4 + copied * 4);
    if (gotWords) *gotWords = copied;
    return scmiStatus;
}

// Walk DESCRIBE_LEVELS: v1-v3 rows are {perf, power, latency}; v4 rows add {indicative_freq, level_index}.
void describe(unsigned d) {
    Domain &dom = gDomain[d];
    const unsigned rowWords = (gVersion >> 16) >= 4 ? 5 : 3;
    uint32_t index = 0;
    dom.minLevel = ~0U;
    for (unsigned round = 0; round < 16; ++round) {
        uint32_t req[2] = {d, index}, resp[96];
        unsigned got = 0;
        int st = scmiCall(kMsgDescribeLevels, req, 2, resp, 96, &got);
        if (st) {gError = st; return;}
        if (!got) return;
        const uint32_t returned = resp[0] & 0xfff, remaining = resp[0] >> 16;
        for (uint32_t i = 0; i < returned && 1 + (i + 1) * rowWords <= got; ++i) {
            const uint32_t perf = resp[1 + i * rowWords];
            if (perf < dom.minLevel) dom.minLevel = perf;
            if (perf > dom.maxLevel) dom.maxLevel = perf;
            // Without a cap take the top level; with one, the highest level whose kHz (perf on qcom) fits.
            if ((!gCapKHz || perf <= gCapKHz) && perf > dom.picked) {
                dom.picked = perf;
                dom.pickedIndex = rowWords == 5 ? resp[1 + i * rowWords + 4] : index + i;
            }
        }
        dom.levels = index + returned;
        if (!remaining || !returned) return;
        index += returned;
    }
}

void measure() {
    uint64_t before[kDomains], after[kDomains];
    for (unsigned d = 0; d < kDomains; ++d) before[d] = gCycle[d] ? rd64(gCycle[d]) : 0;
    const uint64_t t0 = nowUs();
    IOSleep(100);
    const uint64_t dt = nowUs() - t0;
    for (unsigned d = 0; d < kDomains; ++d) {
        after[d] = gCycle[d] ? rd64(gCycle[d]) : 0;
        gDomain[d].mhz = dt ? (uint32_t)((after[d] - before[d]) / dt) : 0;
    }
}

void refreshRow() {
    char *out = native_cpufreq_row;
    const size_t cap = sizeof(native_cpufreq_row);
    unsigned n = (unsigned)snprintf(out, cap, "SCMI v%x e%d", gVersion, gError);
    for (unsigned d = 0; d < kDomains && n < cap; ++d) {
        const Domain &dom = gDomain[d];
        n += (unsigned)snprintf(out + n, cap - n, " d%u:f%x n%u %u-%u p%u v%u s%d/%d l%u %uMHz", d, dom.flags >> 24, dom.levels,
                                dom.minLevel == ~0U ? 0 : dom.minLevel / 1000, dom.maxLevel / 1000, dom.picked / 1000, dom.setValue,
                                dom.limitStatus, dom.setStatus, dom.level / 1000, dom.mhz);
    }
}

void cpufreqThread(void *, wait_result_t) {
    uint32_t resp[16];
    unsigned got = 0;
    gError = scmiCall(kMsgVersion, nullptr, 0, resp, 1, &got);
    if (!gError && got) gVersion = resp[0];
    if (!gError) {
        for (unsigned d = 0; d < kDomains; ++d) {
            Domain &dom = gDomain[d];
            uint32_t req = d;
            dom.setStatus = dom.limitStatus = 1;
            int st = scmiCall(kMsgDomainAttributes, &req, 1, resp, 16, &got);
            if (st) {gError = st; break;}
            dom.flags = resp[0];
            if (got >= 3) dom.sustainedKHz = resp[2];
            describe(d);
            if (gMode >= 2 && dom.picked) {
                uint32_t limits[3] = {d, dom.picked, dom.minLevel == ~0U ? dom.picked : dom.minLevel};
                if (dom.flags & (1U << 31)) dom.limitStatus = scmiCall(kMsgLimitsSet, limits, 3, resp, 0, nullptr);
                // SCMI v4 domains with level indexing (attribute bit 25) take the level index rather than the performance value; on an
                // invalid-parameter answer try the other form.
                const bool indexing = dom.flags & (1U << 25);
                dom.setValue = indexing ? dom.pickedIndex : dom.picked;
                uint32_t level[2] = {d, dom.setValue};
                dom.setStatus = scmiCall(kMsgLevelSet, level, 2, resp, 0, nullptr);
                if (dom.setStatus == -2) {
                    dom.setValue = indexing ? dom.picked : dom.pickedIndex;
                    level[1] = dom.setValue;
                    dom.setStatus = scmiCall(kMsgLevelSet, level, 2, resp, 0, nullptr);
                }
            }
            if (!scmiCall(kMsgLevelGet, &req, 1, resp, 1, &got) && got) dom.level = resp[0];
        }
    }
    native_cpufreq_stage = 2;
    for (;;) {
        measure();
        refreshRow();
        IOSleep(2000);
    }
}
}

extern "C" void erhai_cpufreq_probe(void) {
    unsigned mode = 0, capMHz = 0;
    PE_parse_boot_argn("erhai-cpufreq", &mode, sizeof(mode));
    PE_parse_boot_argn("erhai-cpufreq-cap", &capMHz, sizeof(capMHz));
    if (!mode || gMode) return;
    gMode = mode;
    gCapKHz = capMHz * 1000;
    gShm = mapRange(kShmemBase, kShmemSize, &gShmMap);
    gBell = mapRange(kDoorbellBase, kDoorbellSize, &gBellMap);
    unsigned counters = 1;
    PE_parse_boot_argn("erhai-cpufreq-cnt", &counters, sizeof(counters));
    for (unsigned d = 0; counters && d < kDomains; ++d) {
        const uintptr_t page = mapRange(kCycleBase[d] & ~0xfffULL, 0x1000, &gCycleMap[d]);
        gCycle[d] = page ? page + (kCycleBase[d] & 0xfff) : 0;
    }
    native_cpufreq_stage = 1;
    if (!gShm || !gBell) {snprintf(native_cpufreq_row, sizeof(native_cpufreq_row), "SCMI nomap"); native_cpufreq_stage = 2; return;}
    thread_t thread = nullptr;
    if (kernel_thread_start(cpufreqThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
