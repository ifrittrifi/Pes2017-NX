// See guest_memory_nx.h. Adapted from bo2-360-nx's guest_memory_switch.cpp
// (ReXGlue SDK Switch layer, BSD-3-Clause; third_party/licenses/bo2-sdk-LICENSE):
// the commit dance and the lazy per-view mapping are theirs, the page-protection
// half is left out (this runtime does not watch pages on Switch).
#include "guest_memory_nx.h"

#include <malloc.h>
#include <cstring>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

extern "C"
{
#include <switch.h>
}

namespace
{
    constexpr size_t kPage = 0x1000;

    constexpr uint64_t AlignDown(uint64_t v, uint64_t a) { return v & ~(a - 1); }
    constexpr uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

    struct Chunk
    {
        size_t offset;    // in the mapping
        size_t length;
        void* backing;    // heap block; inaccessible once it is a code alias
        void* shadow;     // the code alias, always read/write
    };

    struct View
    {
        size_t windowOffset;
        size_t mapOffset;
        size_t length;
    };

    struct State
    {
        std::mutex mutex;
        uint8_t* base = nullptr;
        size_t size = 0;
        VirtmemReservation* reservation = nullptr;
        std::vector<View> views;
        std::map<size_t, Chunk> chunks;                  // by mapping offset
        std::set<std::pair<size_t, size_t>> viewMapped;  // (view, chunk offset)
        // Read without the lock (the crash report runs on a faulting thread).
        std::atomic<size_t> committed{ 0 };
        std::atomic<size_t> mapped{ 0 };
        std::atomic<uint32_t> lastResult{ 0 };
    };

    State& S()
    {
        static State s;
        return s;
    }

    // Read on every fault without the lock; set once in Init.
    std::atomic<uint8_t*> g_baseFast{ nullptr };
    std::atomic<size_t> g_sizeFast{ 0 };

    Handle Proc() { return envGetOwnProcessHandle(); }

    Chunk* ChunkAt(State& s, size_t offset)
    {
        auto it = s.chunks.upper_bound(offset);
        if (it == s.chunks.begin()) return nullptr;
        --it;
        Chunk& c = it->second;
        return (offset >= c.offset && offset < c.offset + c.length) ? &c : nullptr;
    }

    bool ViewIndexFor(const State& s, size_t windowOffset, size_t& index)
    {
        for (size_t i = 0; i < s.views.size(); ++i)
        {
            const View& v = s.views[i];
            if (windowOffset >= v.windowOffset && windowOffset < v.windowOffset + v.length)
            {
                index = i;
                return true;
            }
        }
        return false;
    }

    bool Intersect(size_t a, size_t al, size_t b, size_t bl, size_t& lo, size_t& hi)
    {
        lo = a > b ? a : b;
        const size_t ea = a + al, eb = b + bl;
        hi = ea < eb ? ea : eb;
        return lo < hi;
    }

    bool MapChunkIntoView(State& s, size_t viewIndex, const Chunk& c)
    {
        if (s.viewMapped.count({ viewIndex, c.offset })) return true;
        const View& v = s.views[viewIndex];
        size_t lo, hi;
        if (!Intersect(c.offset, c.length, v.mapOffset, v.length, lo, hi)) return false;
        uint8_t* dst = s.base + v.windowOffset + (lo - v.mapOffset);
        const u64 src = reinterpret_cast<u64>(c.shadow) + (lo - c.offset);
        const Result rc = svcMapProcessMemory(dst, Proc(), src, hi - lo);
        if (R_FAILED(rc))
        {
            s.lastResult = rc;
            return false;
        }
        s.mapped += hi - lo;
        s.viewMapped.insert({ viewIndex, c.offset });
        return true;
    }

    // Backs [begin, end) of the mapping (page aligned). Gaps become chunks.
    bool CommitMapping(State& s, size_t begin, size_t end)
    {
        size_t cur = begin;
        while (cur < end)
        {
            if (Chunk* existing = ChunkAt(s, cur))
            {
                cur = existing->offset + existing->length;
                continue;
            }
            size_t gapEnd = end;
            auto next = s.chunks.upper_bound(cur);
            if (next != s.chunks.end() && next->second.offset < gapEnd) gapEnd = next->second.offset;

            const size_t sz = gapEnd - cur;
            const size_t align = (sz >= guest_nx::kGranule && (cur & (guest_nx::kGranule - 1)) == 0)
                                     ? guest_nx::kGranule : kPage;
            void* backing = memalign(align, sz);
            if (!backing)
            {
                s.lastResult = 0xFFFF0001u;
                return false;
            }
            std::memset(backing, 0, sz);

            virtmemLock();
            void* shadow = virtmemFindCodeMemory(sz, kPage);
            Result rc = shadow ? svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                                         reinterpret_cast<u64>(backing), sz)
                               : 0;
            virtmemUnlock();
            if (!shadow)
            {
                free(backing);
                s.lastResult = 0xFFFF0002u;
                return false;
            }
            if (R_FAILED(rc))
            {
                free(backing);
                s.lastResult = rc;
                return false;
            }
            // The code alias starts read/execute; the guest writes through it.
            rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(shadow), sz, Perm_Rw);
            if (R_FAILED(rc))
            {
                svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                          reinterpret_cast<u64>(backing), sz);
                free(backing);
                s.lastResult = rc;
                return false;
            }
            s.chunks[cur] = Chunk{ cur, sz, backing, shadow };
            s.committed += sz;
            cur = gapEnd;
        }
        return true;
    }
}

uint8_t* guest_nx::Init(size_t size)
{
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (s.base) return s.base;
    virtmemLock();
    s.base = static_cast<uint8_t*>(virtmemFindAslr(size, kGranule));
    if (s.base) s.reservation = virtmemAddReservation(s.base, size);
    virtmemUnlock();
    if (!s.base || !s.reservation)
    {
        s.base = nullptr;
        return nullptr;
    }
    s.size = size;
    g_sizeFast.store(size, std::memory_order_release);
    g_baseFast.store(s.base, std::memory_order_release);
    return s.base;
}

bool guest_nx::AddView(size_t windowOffset, size_t mapOffset, size_t length)
{
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (!s.base || windowOffset + length > s.size) return false;
    s.views.push_back(View{ windowOffset, mapOffset, length });
    return true;
}

bool guest_nx::CommitWindow(uint64_t windowOffset, uint64_t length)
{
    if (!length) return true;
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (!s.base) return false;
    uint64_t at = windowOffset;
    const uint64_t end = windowOffset + length;
    while (at < end)
    {
        size_t vi = 0;
        if (!ViewIndexFor(s, size_t(at), vi)) return false;
        const View& v = s.views[vi];
        const uint64_t viewEnd = v.windowOffset + v.length;
        const uint64_t stop = end < viewEnd ? end : viewEnd;
        const uint64_t mapBegin = v.mapOffset + (at - v.windowOffset);
        const uint64_t mapEnd = v.mapOffset + (stop - v.windowOffset);
        // Granule-aligned, clipped to the view's span of the mapping.
        uint64_t b = AlignDown(mapBegin, kGranule), e = AlignUp(mapEnd, kGranule);
        if (b < v.mapOffset) b = v.mapOffset;
        if (e > v.mapOffset + v.length) e = v.mapOffset + v.length;
        if (!CommitMapping(s, size_t(b), size_t(e))) return false;
        at = stop;
    }
    return true;
}

bool guest_nx::InWindow(uint64_t hostAddress)
{
    const uint64_t base = reinterpret_cast<uint64_t>(g_baseFast.load(std::memory_order_acquire));
    return base && hostAddress >= base && hostAddress - base < g_sizeFast.load(std::memory_order_acquire);
}

bool guest_nx::FaultIn(uint64_t hostAddress, bool commitOnTouch)
{
    if (!InWindow(hostAddress)) return false;
    State& s = S();
    std::lock_guard lock(s.mutex);
    const size_t windowOffset = size_t(hostAddress - reinterpret_cast<uint64_t>(s.base));
    size_t vi = 0;
    if (!ViewIndexFor(s, windowOffset, vi)) return false;
    const View& v = s.views[vi];
    const size_t off = v.mapOffset + (windowOffset - v.windowOffset);
    Chunk* c = ChunkAt(s, off);
    if (!c)
    {
        if (!commitOnTouch) return false;
        size_t b = size_t(AlignDown(off, kGranule)), e = b + kGranule;
        if (b < v.mapOffset) b = v.mapOffset;
        if (e > v.mapOffset + v.length) e = v.mapOffset + v.length;
        if (!CommitMapping(s, b, e)) return false;
        c = ChunkAt(s, off);
        if (!c) return false;
    }
    // Already in this view: another thread mapped it between the fault and
    // here. Retry; the handler gives up on an address that keeps faulting.
    if (s.viewMapped.count({ vi, c->offset })) return true;
    return MapChunkIntoView(s, vi, *c);
}

bool guest_nx::EnsureMapped(const void* host, size_t length)
{
    const uint64_t at = reinterpret_cast<uint64_t>(host);
    if (!length || !InWindow(at)) return true;
    State& s = S();
    std::lock_guard lock(s.mutex);
    uint64_t cur = at - reinterpret_cast<uint64_t>(s.base);
    const uint64_t end = cur + length;
    while (cur < end)
    {
        size_t vi = 0;
        if (!ViewIndexFor(s, size_t(cur), vi)) return false;
        const View& v = s.views[vi];
        const size_t off = v.mapOffset + size_t(cur - v.windowOffset);
        Chunk* c = ChunkAt(s, off);
        if (!c) return false;
        if (!MapChunkIntoView(s, vi, *c)) return false;
        cur = v.windowOffset + (c->offset + c->length - v.mapOffset);
    }
    return true;
}

uint8_t* guest_nx::Base() { return g_baseFast.load(std::memory_order_acquire); }

size_t guest_nx::CommittedBytes() { return S().committed.load(std::memory_order_relaxed); }

size_t guest_nx::MappedBytes() { return S().mapped.load(std::memory_order_relaxed); }

uint32_t guest_nx::LastResult() { return S().lastResult.load(std::memory_order_relaxed); }
