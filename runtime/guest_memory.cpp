#include "guest_memory.h"
#include <initializer_list>
#include "log.h"
#include <ppc_config.h>

#if defined(__SWITCH__)
#  include "switch/guest_memory_nx.h"
extern "C"
{
#  include <switch.h>
}
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  include <sys/syscall.h>
#  include <fcntl.h>
#  include <string>
#endif

namespace
{
    uint8_t* g_base = nullptr;
    constexpr uint64_t kSpace = 0x100000000ull;   // full 32-bit guest space
#ifdef __SWITCH__
    // The mapping behind the window (runtime/switch/guest_memory_nx.h): the
    // 512 MB bank at mapping offset 0, shown at all three of its windows; the
    // rest of the guest space one-to-one above it.
    constexpr uint64_t kBankSize = uint64_t(guest::kPhysicalEnd) - guest::kPhysicalBase;
    constexpr uint64_t kPlainOffset = kBankSize;

    void LogSwitchMemoryBudget()
    {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        const AppletType type = appletGetAppletType();
        LOGI("guest: applet type %d; process memory %llu MB total, %llu MB used (the heap holds the rest)",
             int(type), (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
        if (type != AppletType_Application)
            LOGW("guest: running as an applet (about 400 MB); launch through a game (hold R) or a forwarder for the full memory");
    }
#endif
}

uint8_t* guest::Base() { return g_base; }

bool guest::Commit(uint32_t address, uint32_t size)
{
#if defined(__SWITCH__)
    if (!g_base || !size) return false;
    if (uint64_t(address) + size > kSpace)
    {
        LOGE("guest: commit %08X + %08X runs past the 4 GB guest space", address, size);
        return false;
    }
    // Backed on first touch, as the Linux reservation is: the fault handler
    // commits the 2 MB around a touched address. Committing here took every
    // reservation in full (the title reserves 512 MB of far heap twice) and ran
    // the heap dry at kick-off.
    return true;
#else
    // The reservation is already mapped read/write; pages are backed on touch.
    (void)address; (void)size;
    return true;
#endif
}

// On Linux the whole space is mapped read/write and a page is backed the first
// time it is touched, wherever it is. Windows only backs what is committed, so
// the fault handler commits a reserved page the guest touches: the same
// memory, zeroed, on first use.
//
// That includes the first pages. A read through a null guest pointer returns
// zero where the console faults, so a title function whose only hazard is a
// null base needs no guard here: G_GetClientScore (sub_82231F10 in the
// multiplayer) reads level.clients[index].score with level.clients null while
// no game is loaded, which iw4x guards on the PC, and returns 0 on both systems.
bool guest::CommitOnTouch(const void* address)
{
    (void)address;
    return false;
}

namespace
{
    // The physical bank, seen through both of the windows the console gives it.
    //
    // D3D9 forms an address for the command processor as
    // (address & 0x1FFFFFFF) - 0x40000000 -- the same offset in the 0xC0000000
    // window -- and stores it in structures it later reads back itself, the chunk
    // list of a recorded command buffer among them. Here the two windows have to
    // be one piece of memory, or the second is a silent 512 MB of zeroes: the
    // reads succeed, return nothing, and the failure surfaces somewhere else
    // entirely as a count of zero or a null pointer.
    //
    // A shared memory object placed over the reservation twice. Nothing else has
    // to know: a guest pointer through either window is dereferenced as an
    // offset from the base and lands on the same page.
#if defined(__SWITCH__)
    bool ReserveSpace()
    {
        g_base = guest_nx::Init(size_t(kSpace));
        if (!g_base) LOGE("guest: could not reserve a 4 GB window in the address space");
        return g_base != nullptr;
    }

    // Views only: nothing is backed until it is committed, nothing is mapped
    // until it is touched (the exception handler, runtime/switch/exception.cpp).
    bool MapPhysicalAperture()
    {
        bool ok = true;
        // Below the bank, one-to-one: the function table and the far heap.
        ok = ok && guest_nx::AddView(0, size_t(kPlainOffset), size_t(guest::kCachedBase));
        for (uint32_t window : { guest::kCachedBase, guest::kPhysicalBase, guest::kApertureBase })
            ok = ok && guest_nx::AddView(window, 0, size_t(kBankSize));
        // Above the last window, one-to-one again.
        const uint64_t top = uint64_t(guest::kApertureBase) + kBankSize;
        ok = ok && guest_nx::AddView(size_t(top), size_t(kPlainOffset + top), size_t(kSpace - top));
        if (ok) LOGI("guest: 512 MB bank in %zu MB chunks, mapped into each window on first touch",
                     guest_nx::kGranule >> 20);
        return ok;
    }
#else
    bool ReserveSpace()
    {
        // MAP_NORESERVE: address space only, pages allocated on touch.
        g_base = (uint8_t*)mmap(nullptr, kSpace, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (g_base == MAP_FAILED) g_base = nullptr;
        return g_base != nullptr;
    }

    // A file descriptor for the physical bank. memfd_create everywhere it is
    // allowed; on an Android device whose seccomp policy refuses it, an
    // unlinked file in the app's own cache does the same job -- what the two
    // windows need is a descriptor two mappings can share, not a memfd.
    int OpenBankDescriptor()
    {
        const int fd = int(syscall(SYS_memfd_create, "pes17-physical", 0));
        if (fd >= 0) return fd;
        return -1;
    }

    bool MapPhysicalAperture()
    {
        const size_t size = size_t(guest::kPhysicalEnd - guest::kPhysicalBase);
        const int fd = OpenBankDescriptor();
        if (fd < 0) { LOGE("no descriptor for the physical bank"); return false; }
        bool ok = ftruncate(fd, off_t(size)) == 0;
        // All three windows, as on Windows: the heap hands out cached-window
        // addresses (kernel.cpp kWindowDelta) that the GPU and DMA reach by
        // their physical address, so the cached window must be the bank too.
        for (uint32_t window : { guest::kCachedBase, guest::kPhysicalBase, guest::kApertureBase })
        {
            if (!ok) break;
            ok = mmap(g_base + window, size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) != MAP_FAILED;
        }
        // The mappings keep the memory alive; the descriptor is not needed.
        close(fd);
        return ok;
    }
#endif
}

uint8_t* guest::Initialise()
{
#if defined(__SWITCH__)
    LogSwitchMemoryBudget();
    LOGI("host pages are %u bytes", 0x1000u);
#else
    // Worth a line: an Android 15 device has 16 KB pages, everything else
    // has 4 KB, and the difference decides whether memory protection can
    // work at the granularity the renderer wants (gpu/memory_watch.h).
    LOGI("host pages are %ld bytes", sysconf(_SC_PAGESIZE));
#endif
    if (!ReserveSpace())
    {
        LOGE("could not reserve %llu bytes of address space", (unsigned long long)kSpace);
        return nullptr;
    }
    // The bank first: on Windows the rest of the space is only reserved once
    // the views are in place.
    if (!MapPhysicalAperture())
    {
        LOGE("could not alias the physical aperture at %08X", kApertureBase);
        return nullptr;
    }

    const uint32_t funcTableBase = kFuncTableBase;
    const uint32_t funcTableSize = uint32_t(PPC_CODE_SIZE * 2);

    static_assert(PPC_IMAGE_BASE + PPC_IMAGE_SIZE <= kHeapBase,
                  "the heap must start above the image");
    static_assert(kFuncTableBase + PPC_CODE_SIZE * 2 <= kFarBase,
                  "the function lookup table must end below the far heap");

    // On Switch the heap is backed by Heap::Alloc as it hands memory out;
    // committing all of it here would take 415 MB at once.
    if (!Commit(uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE)) ||
        !Commit(funcTableBase, funcTableSize)
#if !defined(__SWITCH__)
        || !Commit(kHeapBase, kHeapEnd - kHeapBase)
#endif
       )
    {
        LOGE("failed to commit guest regions");
        return nullptr;
    }

    LOGI("guest space at host %p", (void*)g_base);
    LOGI("  image      %08X..%08X  (%.1f MB)", uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE), PPC_IMAGE_SIZE / 1048576.0);
    LOGI("  func table %08X..%08X  (%.1f MB)", funcTableBase, funcTableBase + funcTableSize, funcTableSize / 1048576.0);
    LOGI("  heap       %08X..%08X  (%.1f MB; the same bank as the physical window)", kHeapBase, kHeapEnd, (kHeapEnd - kHeapBase) / 1048576.0);
    LOGI("  far heap   %08X..%08X  (%.1f MB of address space, backed on touch)", kFarBase, kFarEnd, (kFarEnd - kFarBase) / 1048576.0);
    LOGI("  physical   %08X..%08X  (%.1f MB)", kPhysicalBase, kPhysicalEnd, (kPhysicalEnd - kPhysicalBase) / 1048576.0);
    LOGI("  aperture   %08X..%08X  (the same memory, second window)",
         kApertureBase, uint32_t(kApertureBase + (kPhysicalEnd - kPhysicalBase)));
    return g_base;
}
