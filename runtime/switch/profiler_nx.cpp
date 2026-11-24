// See profiler_nx.h.
#include "profiler_nx.h"

extern "C"
{
#include <switch.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void _start(void);

namespace
{
    constexpr int kMaxThreads = 96;
    constexpr uint64_t kSampleNs = 3'000'000;          // ~330 samples a second per running thread
    constexpr uint64_t kReportNs = 20'000'000'000ull;
    constexpr uint32_t kTableSize = 1u << 16;
    constexpr uint32_t kLinesPerThread = 40;
    constexpr uint32_t kMinSamples = 30;

    struct Slot
    {
        Handle handle = 0;
        char name[48]{};
        bool active = false;
        uint64_t ticksAtReport = 0;   // thread CPU ticks when the window began
        uint64_t ticksAtSample = 0;
        uint32_t samples = 0, idle = 0, inSvc = 0;
    };

    struct Bucket { uint32_t key, count; };   // key: (slot << 25 | offset >> 2) + 1

    Slot g_slots[kMaxThreads];
    Mutex g_lock;
    Bucket g_table[kTableSize];
    uint32_t g_dropped = 0;
    uint64_t g_base = 0, g_size = 0;
    Thread g_sampler;
    bool g_started = false;

    struct Init { Init() { mutexInit(&g_lock); } } g_init;

    uint64_t ThreadTicks(Handle h)
    {
        u64 ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) return 0;
        return ticks;
    }

    bool InModule(uint64_t a) { return a >= g_base && a - g_base < g_size; }

    void Add(uint32_t slot, uint64_t pc)
    {
        if (!InModule(pc)) return;
        const uint32_t key = ((slot << 25) | uint32_t(((pc - g_base) >> 2) & 0x1FFFFFF)) + 1;
        uint32_t index = (key * 2654435761u) & (kTableSize - 1);
        for (uint32_t probe = 0; probe < 64; ++probe)
        {
            Bucket& b = g_table[(index + probe) & (kTableSize - 1)];
            if (b.key == key) { ++b.count; return; }
            if (!b.key) { b.key = key; b.count = 1; return; }
        }
        ++g_dropped;
    }

    void Write(const std::string& text)
    {
        if (FILE* f = std::fopen("sdmc:/switch/pes2017/profile.txt", "a"))
        {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }

    void Report(uint64_t windowTicks)
    {
        struct Row { uint32_t slot; char name[48]; uint32_t samples, idle, inSvc; double share; };
        std::vector<Row> rows;
        std::vector<Bucket> buckets;

        mutexLock(&g_lock);
        for (uint32_t i = 0; i < kMaxThreads; ++i)
        {
            Slot& s = g_slots[i];
            if (!s.active && !s.samples) continue;
            const uint64_t now = s.active ? ThreadTicks(s.handle) : s.ticksAtReport;
            Row r{};
            r.slot = i;
            std::memcpy(r.name, s.name, sizeof r.name);
            r.samples = s.samples; r.idle = s.idle; r.inSvc = s.inSvc;
            r.share = windowTicks ? 100.0 * double(now - s.ticksAtReport) / double(windowTicks) : 0.0;
            rows.push_back(r);
            s.ticksAtReport = now;
            s.samples = s.idle = s.inSvc = 0;
        }
        for (const Bucket& b : g_table) if (b.key) buckets.push_back(b);
        std::memset(g_table, 0, sizeof g_table);
        const uint32_t dropped = g_dropped;
        g_dropped = 0;
        mutexUnlock(&g_lock);

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.share > b.share; });
        std::sort(buckets.begin(), buckets.end(), [](const Bucket& a, const Bucket& b)
        {
            const uint32_t sa = (a.key - 1) >> 25, sb = (b.key - 1) >> 25;
            return sa != sb ? sa < sb : a.count > b.count;
        });

        std::string out;
        char line[256];
        std::snprintf(line, sizeof line, "==== cpu profile: %.1f s, image 0x%llx, %u samples dropped ====\n",
                      double(windowTicks) / double(armGetSystemTickFreq()), (unsigned long long)g_base, dropped);
        out += line;
        for (const Row& r : rows)
        {
            if (r.share < 0.5 && r.samples < kMinSamples) continue;
            std::snprintf(line, sizeof line, "thread '%s': %.1f%% of a core, %u samples (%u in a system call), %u idle\n",
                          r.name, r.share, r.samples, r.inSvc, r.idle);
            out += line;
            if (r.samples < kMinSamples) continue;
            uint32_t shown = 0;
            for (const Bucket& b : buckets)
            {
                if (((b.key - 1) >> 25) != r.slot) continue;
                if (shown++ == kLinesPerThread) break;
                std::snprintf(line, sizeof line, "  %5.1f%%  image+0x%x\n", 100.0 * b.count / r.samples,
                              ((b.key - 1) & 0x1FFFFFF) << 2);
                out += line;
            }
        }
        out += '\n';
        Write(out);
    }

    void Sampler(void*)
    {
        const Handle self = threadGetCurHandle();
        uint64_t windowStart = armGetSystemTick();
        const uint64_t reportTicks = armNsToTicks(kReportNs);
        for (;;)
        {
            svcSleepThread(kSampleNs);
            mutexLock(&g_lock);
            for (uint32_t i = 0; i < kMaxThreads; ++i)
            {
                Slot& s = g_slots[i];
                if (!s.active || s.handle == self) continue;
                const uint64_t ticks = ThreadTicks(s.handle);
                if (ticks == s.ticksAtSample) { ++s.idle; continue; }
                s.ticksAtSample = ticks;
                // Nothing between pause and resume may allocate, lock or log.
                if (R_FAILED(svcSetThreadActivity(s.handle, ThreadActivity_Paused))) continue;
                ThreadContext context;
                const Result rc = svcGetThreadContext3(&context, s.handle);
                svcSetThreadActivity(s.handle, ThreadActivity_Runnable);
                if (R_FAILED(rc)) continue;
                const uint64_t pc = context.pc.x;
                bool svc = false;
                if (InModule(pc) && !(pc & 3))
                {
                    uint32_t insn;
                    std::memcpy(&insn, reinterpret_cast<const void*>(pc), 4);
                    svc = (insn & 0xFFE0001Fu) == 0xD4000001u;
                }
                // In a system call the caller says more than the svc instruction does.
                Add(i, svc && InModule(context.lr) ? context.lr : pc);
                ++s.samples;
                if (svc) ++s.inSvc;
            }
            mutexUnlock(&g_lock);

            const uint64_t now = armGetSystemTick();
            if (now - windowStart >= reportTicks)
            {
                Report(now - windowStart);
                windowStart = armGetSystemTick();
            }
        }
    }

    int Find(Handle h)
    {
        for (int i = 0; i < kMaxThreads; ++i)
            if (g_slots[i].active && g_slots[i].handle == h) return i;
        return -1;
    }
}

void profiler_nx::RegisterCurrentThread(const char* name)
{
    const Handle h = threadGetCurHandle();
    mutexLock(&g_lock);
    int slot = Find(h);
    if (slot < 0)
        for (int i = 0; i < kMaxThreads; ++i)
            if (!g_slots[i].active && !g_slots[i].samples) { slot = i; break; }
    if (slot >= 0)
    {
        Slot& s = g_slots[slot];
        s.handle = h;
        std::snprintf(s.name, sizeof s.name, "%s", name ? name : "?");
        s.ticksAtReport = s.ticksAtSample = ThreadTicks(h);
        s.active = true;
    }
    mutexUnlock(&g_lock);
}

void profiler_nx::UnregisterCurrentThread()
{
    const Handle h = threadGetCurHandle();
    mutexLock(&g_lock);
    const int slot = Find(h);
    if (slot >= 0)
    {
        g_slots[slot].ticksAtReport = ThreadTicks(h);
        g_slots[slot].active = false;
    }
    mutexUnlock(&g_lock);
}

void profiler_nx::Start()
{
    if (g_started) return;
    const char* value = std::getenv("PES17_PROFILE");
    if (value && value[0] == '0') return;
    g_base = reinterpret_cast<uint64_t>(&_start);
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, g_base))) g_size = info.addr + info.size - g_base;
    // Above every thread it samples, so a sample is never late behind them.
    if (R_FAILED(threadCreate(&g_sampler, Sampler, nullptr, nullptr, 0x10000, 0x1E, -2))) return;
    svcSetThreadCoreMask(g_sampler.handle, -1, 0x7);
    if (R_SUCCEEDED(threadStart(&g_sampler)))
    {
        g_started = true;
        Write("==== PES 2017 profile: a new run ====\n");
    }
}
