#include "../platform.h"

#include "../../switch/source/input.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <cstring>
#include "profiler_nx.h"
#include <cstring>
#include <new>

#include <sys/stat.h>
#include <unistd.h>

// Everything comes out of the libnx heap: the guest memory (memalign,
// runtime/switch/guest_memory_nx.cpp), the host thread stacks and Mesa's GPU
// memory. Started by hbloader the heap is handed over already sized (all of
// the process's memory but ~2 MB) and this value is ignored; started any other
// way, 0 asks libnx for the same.
extern "C"
{
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;
}

// libnx does not expose POSIX thread names, backtraces, or dlopen-compatible
// process services. Keep those operations explicit rather than importing the
// desktop implementation's Linux headers into the Switch target.

namespace
{
    // Cores the title may run on: the process mask without core 3 (the
    // system's). A thread created with core -2 is pinned to the process's
    // default core alone, so every thread used to share core 0.
    u32 AllowedCores()
    {
        static const u32 mask = []
        {
            u64 process = 0;
            if (R_FAILED(svcGetInfo(&process, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !process)
                process = 1;
            const u32 title = u32(process) & 0x7u;
            return title ? title : u32(process) & 0xFu;
        }();
        return mask;
    }

    int NthCore(unsigned n)
    {
        int cores[4];
        int count = 0;
        const u32 mask = AllowedCores();
        for (int i = 0; i < 4; ++i)
            if (mask & (1u << i)) cores[count++] = i;
        return count ? cores[n % unsigned(count)] : 0;
    }

    // Preferred core only; the mask stays all title cores so the kernel can
    // still move a thread off a busy core.
    void PlaceThread(Handle thread, int preferred)
    {
        const u32 mask = AllowedCores();
        if (!(mask & (1u << preferred))) preferred = NthCore(0);
        svcSetThreadCoreMask(thread, preferred, mask);
    }

    // Core 3 when the process was granted it (a forwarder with a 4-core NPDM);
    // -1 otherwise. Only threads pinned there alone run on it.
    int SystemCore()
    {
        static const int core = []
        {
            u64 process = 0;
            if (R_FAILED(svcGetInfo(&process, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) return -1;
            return (process & 0x8u) && (process & 0x7u) ? 3 : -1;
        }();
        return core;
    }

    // Horizon: lower is more urgent. Guest threads run at 0x3B.
    constexpr s32 kRenderPriority = 0x2D;   // GPU ring consumer, presenter, recorder
    constexpr s32 kAudioPriority = 0x2C;    // the mixer: a late frame is an audible gap

    // Core 0 is the main thread's: new threads start on 1, 2, 0, 1, ...
    std::atomic<unsigned> g_nextCore{ 1 };
    std::atomic<unsigned> g_renderThreads{ 0 };

    u64 g_mainThreadId = 0;

    struct ThreadControl
    {
        Thread thread{};
        void (*body)(void*) = nullptr;
        void* argument = nullptr;
        char name[16]{};
    };

    // libnx requires the Thread object and its backing stack/TLS to remain
    // alive until the kernel thread has exited and threadClose has run.
    // The current runtime API is detached and has no join/reap operation, so
    // controls are retained for the process lifetime. This is bounded by the
    // number of runtime worker threads and avoids use-after-free on exit.

    void ThreadEntry(void* raw)
    {
        auto* control = static_cast<ThreadControl*>(raw);
        control->body(control->argument);
    }

    u32 CurrentThreadId()
    {
        u64 id = 0;
        if (R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE))) return 0;
        return static_cast<u32>(id);
    }
}

void platform::Initialise()
{
    g_mainThreadId = CurrentThreadId();


    // The runtime keeps its own VkPipelineCache on SD (vulkan/pipeline.cpp);
    // Mesa's disk cache beside it would only add SD writes during play.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);

    // The runtime's player install layout lives on SD. Mount it before
    // settings, install discovery, or any game files are opened.
    const Result mounted = fsdevMountSdmc();
    if (R_FAILED(mounted))
    {
        constexpr char message[] = "[PES17] failed to mount SD card filesystem\n";
        svcOutputDebugString(message, sizeof(message) - 1);
    }
    else
    {
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/pes2017", 0777);
        // The shader caches live in their own folder; ones an earlier build left beside the
        // game are moved in rather than built again.
        mkdir("sdmc:/switch/pes2017/shader", 0777);
        for (const char* name : { "shader_cache.bin", "pipeline_cache.bin" })
        {
            char from[96], to[96];
            std::snprintf(from, sizeof from, "sdmc:/switch/pes2017/%s", name);
            std::snprintf(to, sizeof to, "sdmc:/switch/pes2017/shader/%s", name);
            struct stat info{};
            if (stat(from, &info) == 0 && stat(to, &info) != 0) std::rename(from, to);
        }
        if (chdir("sdmc:/switch/pes2017") != 0)
        {
            constexpr char message[] = "[PES17] failed to enter sdmc:/switch/pes2017\n";
            svcOutputDebugString(message, sizeof(message) - 1);
        }
    }

    // Settings without a rebuild: sdmc:/switch/pes2017/config.ini (made with its defaults when there is none),
    // KEY=VALUE a line (# or ; starts a comment, [sections] are ignored), read before anything
    // looks at its variables.
    FILE* env = std::fopen("sdmc:/switch/pes2017/config.ini", "r");
    if (!env)
    {
        // First run: write the settings file with its defaults, for the player to edit.
        if (FILE* made = std::fopen("sdmc:/switch/pes2017/config.ini", "w"))
        {
            std::fputs(
                "[log]\npes2017_report=true\npes2017_gpu_timing=true\npes2017_fps_counter=true\npes2017_spike_ms=70\npes2017_auto_dump=false\n\n"
                "[resolution]\npes2017_resolution=0.8\npes2017_upscaler=fsr\npes2017_resolution_adaptive=true\npes2017_resolution_floor=8\npes2017_resolution_budget_ms=22\n\n"
                "[graphics]\npes2017_no_msaa=false\npes2017_shadow_strength=1.5\n;pes2017_no_zcull=false\n\n"
                "[crowd]\npes2017_no_crowd=false\npes2017_crowd_percent=30\npes2017_crowd_adaptive=true\n\n"
                "[clocks]\npes2017_clocks=true\npes2017_cpu_mhz=1224\npes2017_gpu_mhz=614\npes2017_mem_mhz=1600\n\n"
                "[performance]\npes2017_frame_skip=true\n\n"
                "[shader]\n;pes2017_sync_shaders=false\n;pes2017_sync_pipelines=false\n;pes2017_sync_vertex=false\n;pes2017_cache_thread=true\n;pes2017_whole_clears=false\n", made);
            std::fclose(made);
            env = std::fopen("sdmc:/switch/pes2017/config.ini", "r");
        }
    }
    if (env)
    {
        char line[256];
        while (std::fgets(line, sizeof line, env))
        {
            char* text = line;
            while (*text == ' ' || *text == '\t') ++text;
            if (*text == '#' || *text == ';' || *text == '[' || *text == '\0' || *text == '\n' || *text == '\r') continue;
            char* equals = std::strchr(text, '=');
            if (!equals) continue;
            *equals = '\0';
            char* value = equals + 1;
            value[std::strcspn(value, "\r\n")] = '\0';
            for (char* end = equals; end > text && (end[-1] == ' ' || end[-1] == '\t'); --end) end[-1] = '\0';
            while (*value == ' ' || *value == '\t') ++value;
            if (!*text) continue;
            // Lower case keys (pes2017_report=true) are the file's own; true/false stand for 1/0.
            for (char* c = text; *c; ++c) *c = char(std::toupper(static_cast<unsigned char>(*c)));
            // pes2017_xxx is the file's name for what the program reads as PES17_XXX.
            // The file's names for the resolution settings, and the names the program reads them by.
            static const char* const kRenamed[][2] = {
                { "PES2017_RESOLUTION_ADAPTIVE", "PES17_LOAD_SCALE" },
                { "PES2017_RESOLUTION_FLOOR", "PES17_SCALE_FLOOR" },
                { "PES2017_RESOLUTION_BUDGET_MS", "PES17_SCALE_BUDGET_MS" },
                { "PES2017_RESOLUTION", "PES17_SCENE_SCALE" },
            };
            bool renamed = false;
            for (const auto& pair : kRenamed)
                if (std::strcmp(text, pair[0]) == 0) { std::strcpy(text, pair[1]); renamed = true; break; }
            if (!renamed && std::strncmp(text, "PES2017_", 8) == 0) std::memmove(text + 5, text + 8, std::strlen(text + 8) + 1), std::memcpy(text, "PES17", 5);
            for (char* end = value + std::strlen(value); end > value && (end[-1] == ' ' || end[-1] == '\t'); --end) end[-1] = '\0';
            const auto is = [&](const char* word) {
                const char* a = value;
                for (; *a && *word; ++a, ++word)
                    if (std::tolower(static_cast<unsigned char>(*a)) != *word) return false;
                return !*a && !*word;
            };
            if (is("true") || is("on") || is("yes")) value = const_cast<char*>("1");
            else if (is("false") || is("off") || is("no")) value = const_cast<char*>("0");
            setenv(text, value, 1);
        }
        std::fclose(env);
    }
    profiler_nx::Start();

    // Handheld, the GPU runs at 307 MHz and the match is GPU-bound: the scene at 80%
    // (1024x576), brought back to 1280x720 with FSR 1. Docked it stays at 1280x720.
    // PES17_SCENE_SCALE=1 in config.ini keeps the full size handheld too.
    if (appletGetOperationMode() != AppletOperationMode_Console)
    {
        if (!std::getenv("PES17_SCENE_SCALE")) setenv("PES17_SCENE_SCALE", "0.8", 1);
        if (!std::getenv("PES17_UPSCALER") && std::strtod(std::getenv("PES17_SCENE_SCALE"), nullptr) < 1.0)
            setenv("PES17_UPSCALER", "fsr", 1);
    }

    // The console's capture button: homebrew starts with it refused.
    appletSetScreenShotPermission(AppletScreenShotPermission_Enable);

    // Input remains usable even when the SD card is absent or cannot mount.
    switch_input::Initialize();
}

uint32_t platform::ThreadId()
{
    return CurrentThreadId();
}

bool platform::IsMainThread()
{
    return g_mainThreadId != 0 && CurrentThreadId() == g_mainThreadId;
}

void platform::SetThreadName(const char* name)
{
    // Horizon does not provide a public thread-name service through libnx.
    (void)name;
}

void platform::LowerThreadPriority()
{
    // 0x3B is libnx's documented preemptive-multithreading priority.
    svcSetThreadPriority(threadGetCurHandle(), 0x3B);
}

void platform::IdleThreadPriority()
{
    // 0x3F is the lowest priority in the libnx thread priority range.
    svcSetThreadPriority(threadGetCurHandle(), 0x3F);
}

void platform::SetThreadAffinity(ThreadAffinityRole role)
{
    const Handle self = threadGetCurHandle();
    switch (role)
    {
    case ThreadAffinityRole::PrimeCore:
        PlaceThread(self, NthCore(0));
        break;
    case ThreadAffinityRole::RenderCore:
        // The GPU ring consumer first (heavy), then the presenter.
        PlaceThread(self, NthCore(1 + g_renderThreads.fetch_add(1, std::memory_order_relaxed)));
        svcSetThreadPriority(self, kRenderPriority);
        break;
    case ThreadAffinityRole::AudioCore:
        if (SystemCore() >= 0) svcSetThreadCoreMask(self, SystemCore(), 1u << SystemCore());
        else PlaceThread(self, NthCore(2));
        svcSetThreadPriority(self, kAudioPriority);
        break;
    case ThreadAffinityRole::PerformanceCore:
        PlaceThread(self, NthCore(g_nextCore.fetch_add(1, std::memory_order_relaxed)));
        break;
    case ThreadAffinityRole::WorkerCore:
        PlaceThread(self, NthCore(2));
        break;
    case ThreadAffinityRole::Default:
        break;
    }
}

void platform::PrintBacktrace(const char* why)
{
    const char* message = why ? why : "unspecified";
    constexpr char prefix[] = "[PES17] backtrace unavailable on Switch: ";
    svcOutputDebugString(prefix, sizeof(prefix) - 1);
    svcOutputDebugString(message, std::strlen(message));
    svcOutputDebugString("\n", 1);
}

bool platform::StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                           const char* name)
{
    if (!body) return false;

    auto* control = new (std::nothrow) ThreadControl{};
    if (!control) return false;

    control->body = body;
    control->argument = argument;
    if (name) std::snprintf(control->name, sizeof(control->name), "%s", name);

    // libnx allocates stack/TLS storage when stack_mem is NULL. Round the
    // requested guest-worker stack up to a page boundary.
    constexpr size_t kPageSize = 0x1000;
    stackBytes = std::max(stackBytes, size_t(0x4000));
    stackBytes = (stackBytes + kPageSize - 1) & ~(kPageSize - 1);

    Result rc = threadCreate(&control->thread, ThreadEntry, control, nullptr,
                             stackBytes, 0x3B, -2);
    if (R_FAILED(rc))
    {
        delete control;
        return false;
    }
    PlaceThread(control->thread.handle, NthCore(g_nextCore.fetch_add(1, std::memory_order_relaxed)));

    rc = threadStart(&control->thread);
    if (R_FAILED(rc))
    {
        threadClose(&control->thread);
        delete control;
        return false;
    }

    // The control remains allocated because the runtime's detached-thread API
    // has no join/reap callback with which to safely release libnx resources.
    return true;
}

platform::ThreadHandle platform::StartJoinableThread(void (*body)(void*), void* argument,
                                                      size_t stackBytes, const char* name)
{
    if (!body) return nullptr;
    auto* control = new (std::nothrow) ThreadControl{};
    if (!control) return nullptr;
    control->body = body;
    control->argument = argument;
    if (name) std::snprintf(control->name, sizeof(control->name), "%s", name);

    constexpr size_t kPageSize = 0x1000;
    stackBytes = std::max(stackBytes, size_t(0x4000));
    stackBytes = (stackBytes + kPageSize - 1) & ~(kPageSize - 1);
    Result rc = threadCreate(&control->thread, ThreadEntry, control, nullptr,
                             stackBytes, 0x3B, -2);
    if (R_FAILED(rc)) { delete control; return nullptr; }
    PlaceThread(control->thread.handle, NthCore(g_nextCore.fetch_add(1, std::memory_order_relaxed)));
    rc = threadStart(&control->thread);
    if (R_FAILED(rc))
    {
        threadClose(&control->thread);
        delete control;
        return nullptr;
    }
    return control;
}

void platform::JoinThread(ThreadHandle handle)
{
    auto* control = static_cast<ThreadControl*>(handle);
    if (!control) return;
    threadWaitForExit(&control->thread);
    threadClose(&control->thread);
    delete control;
}

void* platform::OpenLibrary(const char* path)
{
    (void)path;
    return nullptr;
}

void* platform::LoadedLibrary(const char* name)
{
    (void)name;
    return nullptr;
}

void* platform::Symbol(void* library, const char* name)
{
    (void)library;
    (void)name;
    return nullptr;
}

// For the periodic log line: docked or handheld, and the clocks the system holds the
// CPU, GPU and memory at (what the frame rate is bounded by, without sys-clk).
namespace switch_clocks
{
    void Describe(char* out, size_t size)
    {
        static bool tried = false, have = false;
        if (!tried)
        {
            tried = true;
            have = R_SUCCEEDED(clkrstInitialize());
        }
        uint32_t hz[3]{};
        if (have)
        {
            const PcvModuleId modules[3] = { PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC };
            for (int i = 0; i < 3; i++)
            {
                ClkrstSession session{};
                if (R_SUCCEEDED(clkrstOpenSession(&session, modules[i], 3)))
                {
                    u32 rate = 0;
                    if (R_SUCCEEDED(clkrstGetClockRate(&session, &rate))) hz[i] = rate;
                    clkrstCloseSession(&session);
                }
            }
        }
        const char* mode = appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld";
        std::snprintf(out, size, "%s, cpu %u MHz, gpu %u MHz, memory %u MHz", mode,
                      hz[0] / 1000000u, hz[1] / 1000000u, hz[2] / 1000000u);
    }

    // The clocks the game needs, set by the game itself (no sys-clk): raised, never lowered. The
    // system puts its own back whenever the performance mode changes, so this is looked at about
    // once a second. PES17_CLOCKS=0 leaves them to sys-clk; PES17_CPU_MHZ / PES17_GPU_MHZ /
    // PES17_MEM_MHZ pick others (0 leaves that one alone).
    void Apply()
    {
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(1)) return;
        last = now;
        static bool tried = false, have = false;
        static uint32_t want[3]{};
        if (!tried)
        {
            tried = true;
            const char* off = std::getenv("PES17_CLOCKS");
            const bool handheld = appletGetOperationMode() != AppletOperationMode_Console;
            auto mhz = [](const char* name, uint32_t fallback) {
                const char* text = std::getenv(name);
                return (text && text[0] ? uint32_t(std::atoi(text)) : fallback) * 1000000u;
            };
            if (!(off && off[0] == '0'))
            {
                want[0] = mhz("PES17_CPU_MHZ", 1785);
                want[1] = mhz("PES17_GPU_MHZ", handheld ? 614 : 768);
                want[2] = mhz("PES17_MEM_MHZ", 1600);
                have = R_SUCCEEDED(clkrstInitialize());
            }
        }
        if (!have) return;
        const PcvModuleId modules[3] = { PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC };
        for (int i = 0; i < 3; i++)
        {
            if (!want[i]) continue;
            ClkrstSession session{};
            if (R_FAILED(clkrstOpenSession(&session, modules[i], 3))) continue;
            u32 rate = 0;
            if (R_SUCCEEDED(clkrstGetClockRate(&session, &rate)) && rate < want[i])
                clkrstSetClockRate(&session, want[i]);
            clkrstCloseSession(&session);
        }
    }
}
