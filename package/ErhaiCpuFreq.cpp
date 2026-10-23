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
//   3  governor: every kTickMs pick each domain's level from its busiest CPU, bounded by a per-domain cap and by a thermal cap that
//      steps one level down while the domain's hottest TSENS sensor is above `erhai-thermal=<C>` and one level up once it is
//      `erhai-thermal-hyst=<C>` below it
// Governor targets follow the stock ColorOS build (init.kernel.post_boot-sun_default_6_2.sh, sys_thermal_control_config.xml):
// WALT with an 80% target load and no rate limits, hispeed 1344 / 2380.8 MHz above 80% load, floors 537.6 / 844.8 MHz, and the
// thermal table's first throttle level (2918.4 / 3072 MHz) as the sustained cap. Stock throttles on a computed shell temperature
// (40-44 C) this driver cannot read yet, so the TSENS target stands in for it. Raises are immediate, decreases one level per tick.
// `erhai-cpufreq-cnt=0` skips the cycle counters.
// TSENS (tsens-v2, live DTB): tsens0 TM 0xc228000 sensors 1-14 are cpu-0-*-* and cpuss-0-*, tsens1 TM 0xc229000 sensors 1-6 are
// cpu-1-*-* and cpuss-1-*; TM_Sn_STATUS at 0xa0 + 4n holds the last reading in bits 11:0 (signed, 0.1 C) and valid in bit 21.
// The status line goes to native_cpufreq_row (the CPU banner row); CpuFreqProbe must stay off (sheng-cpufreq unset).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <mach/processor_info.h>

extern "C" {
extern kern_return_t processor_info(processor_t, processor_flavor_t, host_t *, processor_info_t, mach_msg_type_number_t *);
extern processor_t cpu_to_processor(int);
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
constexpr unsigned kMaxLevels = 32, kCpus = 8, kTickMs = 100, kThermalTicks = 5;
// Logical CPU -> SCMI domain, in the ADT cpu-regs order (0, 0x10000, 0x10100, 0x100 ... 0x500): the 0x1xxxx cores are domain 1.
constexpr unsigned kCpuDomain[kCpus] = {0, 1, 1, 0, 0, 0, 0, 0};
constexpr uint64_t kTsensBase[kDomains] = {0xc228000, 0xc229000};
constexpr unsigned kTsensFirst[kDomains] = {1, 1}, kTsensCount[kDomains] = {14, 6};
constexpr uint32_t kTmStatus = 0xa0;

struct Domain {
    uint32_t flags, sustainedKHz, levels, minLevel, maxLevel, picked, pickedIndex, setValue;
    int setStatus, limitStatus;
    uint32_t level;     // LEVEL_GET after the vote
    uint32_t mhz;       // measured from the cycle counter
    uint32_t khz[kMaxLevels], index[kMaxLevels];   // levels in ascending kHz with the value LEVEL_SET takes for each
    unsigned count, top;     // levels known; highest one under the boot cap
    unsigned floor, hispeed; // governor's lowest level and the level it jumps to above the target load
    unsigned current, thermalTop, util;            // governor state: level in use, thermal ceiling, busiest CPU's load (%)
    int tempDeci;            // hottest sensor (0.1 C)
    unsigned votes;
};
IOMemoryMap *gShmMap, *gBellMap, *gCycleMap[kDomains], *gTsensMap[kDomains];
uintptr_t gShm, gBell, gCycle[kDomains], gTsens[kDomains];
int gThermalDeci = 750, gHystDeci = 50;
uint32_t gFloorKHz[kDomains] = {537600, 844800}, gHispeedKHz[kDomains] = {1344000, 2380800}, gDomainCapKHz[kDomains] = {2918400, 3072000};
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
            if (dom.count < kMaxLevels) {
                // Insert sorted by kHz.
                unsigned at = dom.count++;
                while (at && dom.khz[at - 1] > perf) {dom.khz[at] = dom.khz[at - 1]; dom.index[at] = dom.index[at - 1]; --at;}
                dom.khz[at] = perf;
                dom.index[at] = rowWords == 5 ? resp[1 + i * rowWords + 4] : index + i;
            }
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

bool setLevel(unsigned d, unsigned i) {
    Domain &dom = gDomain[d];
    uint32_t resp[1], level[2] = {d, (dom.flags & (1U << 25)) ? dom.index[i] : dom.khz[i]};
    dom.setStatus = scmiCall(kMsgLevelSet, level, 2, resp, 0, nullptr);
    if (dom.setStatus) return false;
    dom.current = i; dom.setValue = level[1]; dom.votes++;
    return true;
}

int readTemp(unsigned d) {
    int hottest = -2730;
    for (unsigned s = kTsensFirst[d]; gTsens[d] && s < kTsensFirst[d] + kTsensCount[d]; ++s) {
        const uint32_t v = rd32(gTsens[d], kTmStatus + 4 * s);
        if (!(v & (1U << 21))) continue;
        const int t = (int)((v & 0xfff) ^ 0x800) - 0x800;
        if (t > hottest) hottest = t;
    }
    return hottest;
}

// Busy share (%) of each domain's busiest CPU since the previous call.
void sampleLoad(unsigned util[kDomains]) {
    static uint32_t lastBusy[kCpus], lastTotal[kCpus];
    for (unsigned d = 0; d < kDomains; ++d) util[d] = 0;
    for (unsigned c = 0; c < kCpus; ++c) {
        processor_t processor = cpu_to_processor((int)c);
        if (!processor) continue;
        processor_cpu_load_info_data_t info = {};
        mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
        host_t host = nullptr;
        if (processor_info(processor, PROCESSOR_CPU_LOAD_INFO, &host, (processor_info_t)&info, &count) != KERN_SUCCESS) continue;
        const uint32_t busy = info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM] + info.cpu_ticks[CPU_STATE_NICE];
        const uint32_t total = busy + info.cpu_ticks[CPU_STATE_IDLE];
        const uint32_t db = busy - lastBusy[c], dt = total - lastTotal[c];
        lastBusy[c] = busy; lastTotal[c] = total;
        const unsigned u = dt ? db * 100 / dt : 0;
        if (u > util[kCpuDomain[c]]) util[kCpuDomain[c]] = u;
    }
}

void governTick(unsigned tick) {
    unsigned util[kDomains];
    sampleLoad(util);
    for (unsigned d = 0; d < kDomains; ++d) {
        Domain &dom = gDomain[d];
        if (!dom.count) continue;
        dom.util = util[d];
        if (tick % kThermalTicks == 0) {
            dom.tempDeci = readTemp(d);
            if (dom.tempDeci > gThermalDeci && dom.thermalTop > 0) --dom.thermalTop;
            else if (dom.tempDeci < gThermalDeci - gHystDeci && dom.thermalTop < dom.top) ++dom.thermalTop;
        }
        // Lowest level that keeps the busiest CPU at the 80% target (util scaled by the clock it ran at), at least hispeed above it.
        const uint64_t want = (uint64_t)dom.khz[dom.current] * util[d] / 80;
        unsigned i = dom.floor;
        while (i < dom.top && dom.khz[i] < want) ++i;
        if (util[d] > 80 && i < dom.hispeed) i = dom.hispeed;
        if (i < dom.current && dom.current > dom.floor) i = dom.current - 1;    // fall one level per tick
        else if (i < dom.current) i = dom.current;
        if (i > dom.thermalTop) i = dom.thermalTop;
        if (i != dom.current || !dom.votes) setLevel(d, i);
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
        if (gMode >= 3)
            n += (unsigned)snprintf(out + n, cap - n, " d%u:u%u%% %u/%uMHz T%d.%dC cap%u s%d v%u %uMHz", d, dom.util,
                                    dom.count ? dom.khz[dom.current] / 1000 : 0, dom.count ? dom.khz[dom.thermalTop] / 1000 : 0,
                                    dom.tempDeci / 10, (dom.tempDeci < 0 ? -dom.tempDeci : dom.tempDeci) % 10, dom.top,
                                    dom.setStatus, dom.votes, dom.mhz);
        else
            n += (unsigned)snprintf(out + n, cap - n, " d%u:f%x n%u %u-%u p%u v%u s%d/%d l%u %uMHz", d, dom.flags >> 24, dom.levels,
                                    dom.minLevel == ~0U ? 0 : dom.minLevel / 1000, dom.maxLevel / 1000, dom.picked / 1000,
                                    dom.setValue, dom.limitStatus, dom.setStatus, dom.level / 1000, dom.mhz);
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
            const uint32_t capKHz = gMode >= 3 ? gDomainCapKHz[d] : gCapKHz;
            for (dom.top = 0; dom.top + 1 < dom.count && (!capKHz || dom.khz[dom.top + 1] <= capKHz); ++dom.top) {}
            for (dom.floor = 0; dom.floor < dom.top && dom.khz[dom.floor] < gFloorKHz[d]; ++dom.floor) {}
            for (dom.hispeed = dom.floor; dom.hispeed < dom.top && dom.khz[dom.hispeed] < gHispeedKHz[d]; ++dom.hispeed) {}
            dom.current = dom.top;
            dom.thermalTop = dom.top;
            if (gMode == 2 && dom.picked) {
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
    for (unsigned tick = 0;; ++tick) {
        if (gMode >= 3 && !gError) governTick(tick);
        if (tick % 20 == 0) {measure(); refreshRow();}
        IOSleep(kTickMs);
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
    unsigned thermalC = 0, hystC = 0;
    if (PE_parse_boot_argn("erhai-thermal", &thermalC, sizeof(thermalC)) && thermalC) gThermalDeci = (int)thermalC * 10;
    if (PE_parse_boot_argn("erhai-thermal-hyst", &hystC, sizeof(hystC)) && hystC) gHystDeci = (int)hystC * 10;
    // erhai-cpufreq-cap0/1=<MHz> replace the per-domain sustained caps of mode 3 (0 keeps the stock value).
    for (unsigned d = 0; d < kDomains; ++d) {
        char name[24]; unsigned mhz = 0;
        snprintf(name, sizeof(name), "erhai-cpufreq-cap%u", d);
        if (PE_parse_boot_argn(name, &mhz, sizeof(mhz)) && mhz) gDomainCapKHz[d] = mhz * 1000;
        gTsens[d] = mapRange(kTsensBase[d], 0x1000, &gTsensMap[d]);
    }
    native_cpufreq_stage = 1;
    if (!gShm || !gBell) {snprintf(native_cpufreq_row, sizeof(native_cpufreq_row), "SCMI nomap"); native_cpufreq_stage = 2; return;}
    thread_t thread = nullptr;
    if (kernel_thread_start(cpufreqThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
