// Guest memory on Horizon: one reserved window, committed in chunks from the
// libnx heap, mapped into each view on first touch.
//
// Adapted from bo2-360-nx (sdk/src/core/guest_memory_switch.*, ReXGlue SDK
// Switch layer, BSD-3-Clause, see third_party/licenses/bo2-sdk-LICENSE).
//
// Why it is built this way (measured on the console by bo2-360-nx):
//   - Started by hbloader, libnx's heap already holds all of the process's
//     memory but ~2 MB. svcCreateSharedMemory / svcMapPhysicalMemory then fail
//     with 0x10801 (resource limit). So guest memory comes from the heap.
//   - svcMapProcessMemory refuses heap pages (0xD401); the heap block is first
//     turned into a code alias (svcMapProcessCodeMemory) and that alias is
//     mirrored into the window.
//   - Mapping every chunk into all three bank windows at commit time exhausts
//     the process's mappable-memory limit (~1.8 GB, 2001-0103). A chunk enters
//     a view only when that view is touched; the exception handler does it.
#pragma once
#include <cstddef>
#include <cstdint>

namespace guest_nx
{
    // Reserves `size` bytes of address space. Returns the base or null.
    uint8_t* Init(size_t size);

    // The window [windowOffset, windowOffset + length) shows the mapping from
    // `mapOffset` on. Several views may share a mapping offset (the mirrors).
    bool AddView(size_t windowOffset, size_t mapOffset, size_t length);

    // Backs [windowOffset, windowOffset + length) of the window (through its
    // view). Idempotent. Rounded out to kGranule.
    bool CommitWindow(uint64_t windowOffset, uint64_t length);

    // Maps the chunk behind a faulting host address into the view it falls in,
    // committing it first if `commitOnTouch` and nothing is there yet (the
    // window reads as zero-filled memory, as the Linux mmap does).
    // True when the access can be retried.
    bool FaultIn(uint64_t hostAddress, bool commitOnTouch);

    // Maps every committed chunk under [host, host + length) into its view now,
    // for memory handed to a system call (which cannot fault it in).
    bool EnsureMapped(const void* host, size_t length);

    bool InWindow(uint64_t hostAddress);
    uint8_t* Base();

    size_t CommittedBytes();
    size_t MappedBytes();
    uint32_t LastResult();

    inline constexpr size_t kGranule = 0x200000;   // 2 MB: block descriptors, few chunks
}
