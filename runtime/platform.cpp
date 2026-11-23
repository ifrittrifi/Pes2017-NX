#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <new>

#include <climits>
#include <dlfcn.h>
// bionic has no execinfo.h: Android's unwinder is reached another way, and
// the backtrace below is written by hand there.
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>

void platform::Initialise() {}
uint32_t platform::ThreadId() { return uint32_t(gettid()); }
bool platform::IsMainThread() { return gettid() == getpid(); }

void platform::SetThreadName(const char* name)
{
    char shortName[16];
    std::snprintf(shortName, sizeof shortName, "%s", name);
    pthread_setname_np(pthread_self(), shortName);
}

void platform::LowerThreadPriority() { setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10); }

void platform::IdleThreadPriority()
{
    sched_param none{};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &none) != 0) LowerThreadPriority();
}

namespace
{
    struct CpuTopology
    {
        uint32_t primeMask = 0;
        uint32_t perfMask = 0;
        uint32_t workerMask = 0;
        bool discovered = false;
    };

    CpuTopology DiscoverCpuTopology()
    {
        CpuTopology topo;
        const long cpus = sysconf(_SC_NPROCESSORS_CONF);
        if (cpus <= 1) return topo;

        const int maxCpus = std::min(int(cpus), 32);
        struct CoreFreq { int id; unsigned long freq; };
        std::vector<CoreFreq> freqs;

        for (int i = 0; i < maxCpus; ++i)
        {
            char path[128];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
            FILE* f = std::fopen(path, "r");
            unsigned long freq = 0;
            if (f)
            {
                if (std::fscanf(f, "%lu", &freq) == 1) freqs.push_back({ i, freq });
                std::fclose(f);
            }
        }

        if (freqs.size() == size_t(maxCpus) && !freqs.empty())
        {
            std::sort(freqs.begin(), freqs.end(), [](const CoreFreq& a, const CoreFreq& b) {
                return a.freq > b.freq;
            });

            unsigned long maxFreq = freqs[0].freq;
            for (const auto& c : freqs)
            {
                if (c.freq >= maxFreq * 95 / 100)
                    topo.primeMask |= (1u << c.id);
                else if (c.freq >= maxFreq * 75 / 100)
                    topo.perfMask |= (1u << c.id);
                else
                    topo.workerMask |= (1u << c.id);
            }
            if (!topo.perfMask) topo.perfMask = topo.primeMask;
            if (!topo.workerMask) topo.workerMask = topo.perfMask;
        }
        else
        {
            if (maxCpus >= 8)
            {
                topo.primeMask = (1u << 7);
                topo.perfMask = (1u << 4) | (1u << 5) | (1u << 6);
                topo.workerMask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
            }
            else if (maxCpus >= 4)
            {
                topo.primeMask = (1u << (maxCpus - 1));
                for (int i = maxCpus / 2; i < maxCpus; ++i) topo.perfMask |= (1u << i);
                for (int i = 0; i < maxCpus / 2; ++i) topo.workerMask |= (1u << i);
            }
            else
            {
                topo.primeMask = 1u;
                topo.perfMask = 1u;
                topo.workerMask = 1u;
            }
        }

        topo.discovered = true;
        return topo;
    }
}

static bool IsMaliDevice()
{
    static const bool mali = [] {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("ro.hardware.vulkan", value);
        return std::strcmp(value, "mali") == 0;
    }();
    return mali;
}

void platform::SetThreadAffinity(ThreadAffinityRole role)
{
    if (role == ThreadAffinityRole::Default) return;
    static CpuTopology s_topo = DiscoverCpuTopology();
    if (!s_topo.discovered) return;

    // The ring consumer and the presenter are always taken off the prime core, unless
    // PES17_PINNING=shared: they are created from the main thread, which is pinned there,
    // and inherit it, so the consumer ran on the core the main thread spins on. On the POCO
    // X7 Pro a match load measured the consumer waiting 45 s of 58 in the run queue, and the
    // game at 1-3 fps for the minute and a half after kick-off; moved off, the same macro
    // held 60 fps.
    //
    // PES17_PINNING: "off" pins nothing; "wide" (or its old name "split") gives the guest's
    // threads every core but the prime one. By default the guest's threads
    // share the performance cluster, which on a 1+3+4 part
    // is three cores for some sixty threads: on the POCO X7 Pro (Dimensity 8400, whose
    // "little" cores are 2.1 GHz A725s) the busy ones waited about as long as they ran.
    static const char* const s_pinning = [] {
        const char* value = std::getenv("PES17_PINNING");
        return value ? value : "";
    }();
    if (std::strcmp(s_pinning, "off") == 0 || std::strcmp(s_pinning, "0") == 0) return;
    const bool shared = std::strcmp(s_pinning, "shared") == 0;
    const bool wide = std::strcmp(s_pinning, "split") == 0 || std::strcmp(s_pinning, "wide") == 0;
    // "big": the guest's threads and the ring consumer/presenter share the performance
    // cores with the prime one. On the POCO X7 Pro at a 60 limit the three performance
    // cores ran ~80% busy and the ring consumer waited in their run queue longer than it
    // ran, while the prime core, the main thread's, sat ~55% idle.
    const bool big = std::strcmp(s_pinning, "big") == 0;

    uint32_t targetMask = 0;
    switch (role)
    {
    case ThreadAffinityRole::PrimeCore:
        targetMask = s_topo.primeMask ? s_topo.primeMask : s_topo.perfMask;
        break;
    case ThreadAffinityRole::PerformanceCore:
        targetMask = s_topo.perfMask ? s_topo.perfMask : s_topo.primeMask;
        if (wide && s_topo.workerMask != s_topo.perfMask) targetMask |= s_topo.workerMask;
        if (big) targetMask |= s_topo.primeMask;
        break;
    case ThreadAffinityRole::WorkerCore:
        targetMask = s_topo.workerMask ? s_topo.workerMask : s_topo.perfMask;
        break;
    case ThreadAffinityRole::RenderCore:
        // Only where measured: on the S20 FE (Adreno) pinning these to the performance
        // cluster cost the match its steady 60 fps (43-53 with a wide spread), so there
        // they inherit the main thread's mask as before. PES17_PINNING=render forces it.
        if (shared) return;
        if (std::strcmp(s_pinning, "render") != 0 && !big && !IsMaliDevice()) return;
        targetMask = s_topo.perfMask ? s_topo.perfMask : s_topo.primeMask;
        if (big) targetMask |= s_topo.primeMask;
        break;
    default:
        return;
    }

    if (!targetMask) return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 32; ++i)
    {
        if (targetMask & (1u << i))
            CPU_SET(i, &cpuset);
    }
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

void platform::PrintBacktrace(const char* why)
{
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fprintf(stderr, "\n[E] ---- %s (%d frames) ----\n", why, n);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, n, 2);
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
}

namespace
{
    struct ThreadStart { void (*body)(void*); void* argument; const char* name; };

    void* RunThread(void* raw)
    {
        ThreadStart start = *static_cast<ThreadStart*>(raw);
        delete static_cast<ThreadStart*>(raw);
        if (start.name) platform::SetThreadName(start.name);
        start.body(start.argument);
        return nullptr;
    }
}

bool platform::StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                           const char* name)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stackBytes)
    {
        // Rounded up to a page, and never below what the platform insists on.
        const size_t page = size_t(sysconf(_SC_PAGESIZE));
        size_t wanted = (stackBytes + page - 1) & ~(page - 1);
#ifdef PTHREAD_STACK_MIN
        if (wanted < size_t(PTHREAD_STACK_MIN)) wanted = size_t(PTHREAD_STACK_MIN);
#endif
        pthread_attr_setstacksize(&attributes, wanted);
    }
    auto* start = new ThreadStart{ body, argument, name };
    pthread_t thread{};
    const int made = pthread_create(&thread, &attributes, &RunThread, start);
    pthread_attr_destroy(&attributes);
    if (made != 0)
    {
        delete start;
        // A smaller stack is worth trying before giving up: a device short of
        // address space would rather have the thread than the headroom.
        pthread_t fallback{};
        auto* retry = new ThreadStart{ body, argument, name };
        if (pthread_create(&fallback, nullptr, &RunThread, retry) != 0)
        {
            delete retry;
            return false;
        }
        pthread_detach(fallback);
    }
    return true;
}

namespace
{
    struct JoinableThread { pthread_t thread{}; };
}

platform::ThreadHandle platform::StartJoinableThread(void (*body)(void*), void* argument,
                                                      size_t stackBytes, const char* name)
{
    if (!body) return nullptr;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return nullptr;
    if (stackBytes)
    {
        const size_t page = size_t(sysconf(_SC_PAGESIZE));
        size_t wanted = (stackBytes + page - 1) & ~(page - 1);
#ifdef PTHREAD_STACK_MIN
        if (wanted < size_t(PTHREAD_STACK_MIN)) wanted = size_t(PTHREAD_STACK_MIN);
#endif
        if (pthread_attr_setstacksize(&attributes, wanted) != 0)
        {
            pthread_attr_destroy(&attributes);
            return nullptr;
        }
    }
    auto* start = new (std::nothrow) ThreadStart{ body, argument, name };
    auto* handle = new (std::nothrow) JoinableThread{};
    if (!start || !handle)
    {
        delete start;
        delete handle;
        pthread_attr_destroy(&attributes);
        return nullptr;
    }
    const int made = pthread_create(&handle->thread, &attributes, &RunThread, start);
    pthread_attr_destroy(&attributes);
    if (made != 0)
    {
        delete start;
        delete handle;
        return nullptr;
    }
    return handle;
}

void platform::JoinThread(ThreadHandle raw)
{
    auto* handle = static_cast<JoinableThread*>(raw);
    if (!handle) return;
    pthread_join(handle->thread, nullptr);
    delete handle;
}

void* platform::OpenLibrary(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* platform::LoadedLibrary(const char* name) { return dlopen(name, RTLD_NOW | RTLD_NOLOAD); }
void* platform::Symbol(void* library, const char* name) { return dlsym(library, name); }

