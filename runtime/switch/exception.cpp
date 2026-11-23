// Faults on Horizon: lazy guest memory and readable crash reports.
//
// There are no signals on Horizon. A fault reaches __libnx_exception_handler on
// an exception stack, and returning from it kills the process. The entry and
// resume assembly below are taken verbatim from bo2-360-nx
// (sdk/src/core/exception_handler_switch.cpp and resume_ctx.h; ReXGlue SDK
// Switch layer, BSD-3-Clause, third_party/licenses/bo2-sdk-LICENSE):
//   - eight exception stack/dump slots, so faults on several threads at once
//     do not overwrite each other (libnx has one global stack and dump);
//   - resume through the kernel: the corrected dump is copied below the
//     faulting thread's sp, a udf trap is raised, and the entry restores
//     x0-x8/lr/sp/pc/pstate from it with svcReturnFromException.
//
// What this runtime does with a fault:
//   1. inside the guest window: map the chunk into the view that was touched
//      (committing it first when nothing is there: the window reads as zeroed
//      memory, as the Linux build's MAP_NORESERVE reservation does), retry;
//   2. anything else: write registers and a stack scan to
//      sdmc:/switch/PES2017/crash.log and stop with an error on screen.
#include "guest_memory_nx.h"

#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C"
{
#include <switch.h>
}

extern "C"
{
    // libnx reads this from the entry: keep handling faults with a debugger attached.
    u32 __nx_exception_ignoredebug = 1;

    alignas(16) __attribute__((used)) uint8_t g_rex_exc_stacks[8][0x10000];
    alignas(16) __attribute__((used)) uint8_t g_rex_exc_dumps[8][0x400];
    alignas(16) __attribute__((used)) uint32_t g_rex_exc_occupied[8];

    __attribute__((noreturn)) void RexResumeFromException(ThreadExceptionDump* ctx);
    void _start(void);
}

static_assert(sizeof(ThreadExceptionDump) <= 0x400, "the dump no longer fits in the slot");

static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == 16, "gprs moved");
static_assert(sizeof(CpuRegister) == 8, "CpuRegister is no longer 8 bytes");
static_assert(offsetof(ThreadExceptionDump, fp) == 248, "fp moved");
static_assert(offsetof(ThreadExceptionDump, lr) == 256, "lr moved");
static_assert(offsetof(ThreadExceptionDump, sp) == 264, "sp moved");
static_assert(offsetof(ThreadExceptionDump, pc) == 272, "pc moved");
static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == 288, "NEON moved");
static_assert(sizeof(FpuRegister) == 16, "FpuRegister is no longer 16 bytes");
static_assert(offsetof(ThreadExceptionDump, pstate) == 800, "pstate moved");
static_assert(offsetof(ThreadExceptionDump, far) == 816, "far moved");
static_assert(sizeof(ThreadExceptionDump) <= 0x340, "the dump no longer fits in the copy");
/* The frame the kernel restores, the one the exception entry writes. */
static_assert(offsetof(ThreadExceptionFrameA64, lr) == 72, "lr of the frame moved");
static_assert(offsetof(ThreadExceptionFrameA64, sp) == 80, "sp of the frame moved");
static_assert(offsetof(ThreadExceptionFrameA64, elr_el1) == 88, "pc of the frame moved");
static_assert(offsetof(ThreadExceptionFrameA64, pstate) == 96, "pstate of the frame moved");


__asm__(".text\n"
        ".balign 4\n"
        ".global __libnx_exception_entry\n"
        ".type __libnx_exception_entry, %function\n"
        "__libnx_exception_entry:\n"
        "    cbz  x1, .Lrex_exc_abort\n"
        "    ldr  x2, [x1, #88]\n"
        "    adrp x3, RexResumeTrap\n"
        "    add  x3, x3, :lo12:RexResumeTrap\n"
        "    cmp  x2, x3\n"
        "    b.eq .Lrex_exc_resume\n"
        /* debugger? svcGetInfo(InfoType_DebuggerAttached) preserving x9-x21 */
        "    stp  x9,  x10, [sp, #-16]!\n"
        "    stp  x11, x12, [sp, #-16]!\n"
        "    stp  x13, x14, [sp, #-16]!\n"
        "    stp  x15, x16, [sp, #-16]!\n"
        "    stp  x17, x18, [sp, #-16]!\n"
        "    stp  x19, x20, [sp, #-16]!\n"
        "    str  x21, [sp, #-16]!\n"
        "    stp  x0,  x1,  [sp, #-16]!\n"
        "    sub  sp, sp, #16\n"
        "    mov  x0, sp\n"
        "    mov  x1, #8\n"
        "    mov  w2, wzr\n"
        "    mov  x3, #0\n"
        "    bl   svcGetInfo\n"
        "    mov  w6, w0\n"
        "    ldr  x7, [sp], #16\n"
        "    ldp  x0,  x1,  [sp], #16\n"
        "    ldr  x21, [sp], #16\n"
        "    ldp  x19, x20, [sp], #16\n"
        "    ldp  x17, x18, [sp], #16\n"
        "    ldp  x15, x16, [sp], #16\n"
        "    ldp  x13, x14, [sp], #16\n"
        "    ldp  x11, x12, [sp], #16\n"
        "    ldp  x9,  x10, [sp], #16\n"
        "    cbnz w6, .Lrex_exc_abort\n"
        "    adrp x6, __nx_exception_ignoredebug\n"
        "    ldr  w5, [x6, :lo12:__nx_exception_ignoredebug]\n"
        "    cbnz w5, .Lrex_exc_dump\n"
        "    cbnz x7, .Lrex_exc_abort\n"
        /*
         * Take a free slot. Only x3-x7 are touched: x0 holds the exception type and x1
         * the kernel frame, and both are needed below.
         */
        ".Lrex_exc_dump:\n"
        "    adrp x6, g_rex_exc_occupied\n"
        "    add  x6, x6, :lo12:g_rex_exc_occupied\n"
        "    mov  w7, wzr\n"
        ".Lrex_exc_take:\n"
        "    add  x5, x6, x7, lsl #2\n"
        "    ldaxr w4, [x5]\n"
        "    cbnz w4, .Lrex_exc_next\n"
        "    mov  w3, #1\n"
        "    stlxr w4, w3, [x5]\n"
        "    cbz  w4, .Lrex_exc_caught\n"
        "    b    .Lrex_exc_take\n"
        ".Lrex_exc_next:\n"
        "    clrex\n"
        "    add  w7, w7, #1\n"
        "    cmp  w7, #8\n"
        "    b.lo .Lrex_exc_take\n"
        /*
         * All taken: go back to the first one and wait. A thread stalled here is far
         * less harmful than two writing to the same dump.
         */
        "    mov  w7, wzr\n"
        "    b    .Lrex_exc_take\n"
        ".Lrex_exc_caught:\n"
        /*
         * This slot's dump, with the slot number in pad[0], which the resume branch
         * reads from the copy to release it.
         */
        "    adrp x2, g_rex_exc_dumps\n"
        "    add  x2, x2, :lo12:g_rex_exc_dumps\n"
        "    add  x2, x2, x7, lsl #10\n"
        "    mov  x5, x2\n"
        "    str  w0,  [x2], #4\n"
        "    str  w7,  [x2], #4\n"
        "    str  wzr, [x2], #4\n"
        "    str  wzr, [x2], #4\n"
        "    ldp  x3, x4, [x1]\n"
        "    str  x5, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    str  x9, [x2], #8\n"
        "    stp  x10, x11, [x2], #16\n"
        "    stp  x12, x13, [x2], #16\n"
        "    stp  x14, x15, [x2], #16\n"
        "    stp  x16, x17, [x2], #16\n"
        "    stp  x18, x19, [x2], #16\n"
        "    stp  x20, x21, [x2], #16\n"
        "    stp  x22, x23, [x2], #16\n"
        "    stp  x24, x25, [x2], #16\n"
        "    stp  x26, x27, [x2], #16\n"
        "    str  x28, [x2], #8\n"
        "    str  x29, [x2], #8\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        /* Top of this slot's stack: base + (slot + 1) * 64 KB. */
        "    adrp x4, g_rex_exc_stacks\n"
        "    add  x4, x4, :lo12:g_rex_exc_stacks\n"
        "    add  x7, x7, #1\n"
        "    add  x4, x4, x7, lsl #16\n"
        "    ldr  x3, [x1]\n"
        "    str  x4, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    adrp x4, RexExceptionReturnEntry\n"
        "    add  x4, x4, :lo12:RexExceptionReturnEntry\n"
        "    ldr  x3, [x1]\n"
        "    str  x4, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    str  xzr, [x2], #8\n"
        "    stp  q0,  q1,  [x2], #32\n"
        "    stp  q2,  q3,  [x2], #32\n"
        "    stp  q4,  q5,  [x2], #32\n"
        "    stp  q6,  q7,  [x2], #32\n"
        "    stp  q8,  q9,  [x2], #32\n"
        "    stp  q10, q11, [x2], #32\n"
        "    stp  q12, q13, [x2], #32\n"
        "    stp  q14, q15, [x2], #32\n"
        "    stp  q16, q17, [x2], #32\n"
        "    stp  q18, q19, [x2], #32\n"
        "    stp  q20, q21, [x2], #32\n"
        "    stp  q22, q23, [x2], #32\n"
        "    stp  q24, q25, [x2], #32\n"
        "    stp  q26, q27, [x2], #32\n"
        "    stp  q28, q29, [x2], #32\n"
        "    stp  q30, q31, [x2], #32\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    mov  w0, wzr\n"
        "    b    .Lrex_exc_end\n"
        /* resume: frame <- copied dump (x8 of the frame) */
        ".Lrex_exc_resume:\n"
        "    ldr  x2, [x1, #64]\n"
        /*
         * Release the slot. The thread no longer runs on the exception stack here
         * (it is inside the kernel, about to return to the faulting code), so another
         * thread can take it without overwriting anything. The mask is in case the
         * copied dump were damaged.
         */
        "    ldr  w3, [x2, #4]\n"
        "    and  w3, w3, #7\n"
        "    adrp x4, g_rex_exc_occupied\n"
        "    add  x4, x4, :lo12:g_rex_exc_occupied\n"
        "    add  x4, x4, x3, lsl #2\n"
        "    stlr wzr, [x4]\n"
        "    ldp  x3, x4, [x2, #16]\n"
        "    stp  x3, x4, [x1, #0]\n"
        "    ldp  x3, x4, [x2, #32]\n"
        "    stp  x3, x4, [x1, #16]\n"
        "    ldp  x3, x4, [x2, #48]\n"
        "    stp  x3, x4, [x1, #32]\n"
        "    ldp  x3, x4, [x2, #64]\n"
        "    stp  x3, x4, [x1, #48]\n"
        "    ldr  x3, [x2, #80]\n"
        "    str  x3, [x1, #64]\n"
        "    ldp  x3, x4, [x2, #256]\n"
        "    stp  x3, x4, [x1, #72]\n"
        "    ldr  x3, [x2, #272]\n"
        "    str  x3, [x1, #88]\n"
        "    ldr  w3, [x2, #800]\n"
        "    str  w3, [x1, #96]\n"
        "    mov  w0, wzr\n"
        "    b    .Lrex_exc_end\n"
        ".Lrex_exc_abort:\n"
        "    mov  w0, #0xf801\n"
        ".Lrex_exc_end:\n"
        "    bl   svcReturnFromException\n"
        "    b    .\n"
        ".size __libnx_exception_entry, .-__libnx_exception_entry\n"
        "\n"
        ".balign 4\n"
        ".type RexExceptionReturnEntry, %function\n"
        "RexExceptionReturnEntry:\n"
        "    bl   __libnx_exception_handler\n"
        "    mov  w0, wzr\n"
        "    mov  x1, #0\n"
        "    mov  x2, #0\n"
        "    bl   svcBreak\n"
        "    b    .\n"
        ".size RexExceptionReturnEntry, .-RexExceptionReturnEntry\n");

__asm__(
    ".text\n"
    ".balign 4\n"
    ".global RexResumeFromException\n"
    ".type RexResumeFromException, %function\n"
    "RexResumeFromException:\n"
    /*
     * 1. copy of the dump to the thread's stack: 0x340 bytes, 0x400 below its sp,
     *    16-byte aligned. It reads 8 bytes past the end of the dump, which is memory
     *    of the executable image and can be read.
     */
    "    ldr  x9,  [x0, #264]\n"
    "    sub  x9,  x9, #0x400\n"
    "    bic  x9,  x9, #0xf\n"
    "    mov  x10, x0\n"
    "    mov  x11, x9\n"
    "    add  x12, x9, #0x340\n"
    "1:  ldp  x13, x14, [x10], #16\n"
    "    stp  x13, x14, [x11], #16\n"
    "    cmp  x11, x12\n"
    "    b.lo 1b\n"
    "    mov  x8,  x9\n"
    /* 2. full NEON: the faulting access may have been floating point */
    "    add  x1,  x8, #288\n"
    "    ldp  q0,  q1,  [x1, #0]\n"
    "    ldp  q2,  q3,  [x1, #32]\n"
    "    ldp  q4,  q5,  [x1, #64]\n"
    "    ldp  q6,  q7,  [x1, #96]\n"
    "    ldp  q8,  q9,  [x1, #128]\n"
    "    ldp  q10, q11, [x1, #160]\n"
    "    ldp  q12, q13, [x1, #192]\n"
    "    ldp  q14, q15, [x1, #224]\n"
    "    ldp  q16, q17, [x1, #256]\n"
    "    ldp  q18, q19, [x1, #288]\n"
    "    ldp  q20, q21, [x1, #320]\n"
    "    ldp  q22, q23, [x1, #352]\n"
    "    ldp  q24, q25, [x1, #384]\n"
    "    ldp  q26, q27, [x1, #416]\n"
    "    ldp  q28, q29, [x1, #448]\n"
    "    ldp  q30, q31, [x1, #480]\n"
    /* x9-x28 and fp, including x16, x17 and x18 */
    "    ldp  x9,  x10, [x8, #88]\n"
    "    ldp  x11, x12, [x8, #104]\n"
    "    ldp  x13, x14, [x8, #120]\n"
    "    ldp  x15, x16, [x8, #136]\n"
    "    ldp  x17, x18, [x8, #152]\n"
    "    ldp  x19, x20, [x8, #168]\n"
    "    ldp  x21, x22, [x8, #184]\n"
    "    ldp  x23, x24, [x8, #200]\n"
    "    ldp  x25, x26, [x8, #216]\n"
    "    ldp  x27, x28, [x8, #232]\n"
    "    ldr  x29, [x8, #248]\n"
    /* 3. the trap. x0-x8, lr, sp and pc are set by the kernel. */
    ".global RexResumeTrap\n"
    "RexResumeTrap:\n"
    "    udf  #0x5245\n"
    ".size RexResumeFromException, .-RexResumeFromException\n");

namespace
{
    std::atomic<uint64_t> g_faultsIn{ 0 };

    struct CrashBuf
    {
        char data[16384];
        size_t len = 0;
    };

    __attribute__((format(printf, 2, 3))) void Append(CrashBuf& b, const char* fmt, ...)
    {
        if (b.len + 1 >= sizeof(b.data)) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(b.data + b.len, sizeof(b.data) - b.len, fmt, ap);
        va_end(ap);
        if (n > 0) b.len = (b.len + size_t(n) < sizeof(b.data) - 1) ? b.len + size_t(n) : sizeof(b.data) - 1;
    }

    bool QueryReadable(uint64_t addr, size_t bytes, MemoryInfo* out)
    {
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(out, &pageInfo, addr))) return false;
        return out->type != MemType_Unmapped && (out->perm & Perm_R) && addr + bytes <= out->addr + out->size;
    }

    // Written with the fs service directly: the faulting thread may hold newlib's locks.
    void WriteCrashFile(const char* data, size_t len)
    {
        FsFileSystem* fs = nullptr;
        char path[FS_MAX_PATH];
        if (fsdevTranslatePath("sdmc:/switch/PES2017/crash.log", &fs, path) < 0 || !fs) return;
        fsFsCreateFile(fs, path, 0, 0);
        FsFile f;
        if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &f))) return;
        s64 size = 0;
        fsFileGetSize(&f, &size);
        fsFileWrite(&f, size, data, len, FsWriteOption_Flush);
        fsFileClose(&f);
    }

    void CrashLog(const char* reason, const ThreadExceptionDump* ctx)
    {
        static std::atomic_flag busy = ATOMIC_FLAG_INIT;
        if (busy.test_and_set()) return;
        static CrashBuf b;
        b.len = 0;
        const uint64_t base = reinterpret_cast<uint64_t>(&_start);
        uint64_t textLo = base, textHi = base;
        MemoryInfo mi;
        if (QueryReadable(base, 4, &mi)) { textLo = mi.addr; textHi = mi.addr + mi.size; }
        u64 tid = 0;
        svcGetThreadId(&tid, CUR_THREAD_HANDLE);
        const uint64_t pc = ctx ? ctx->pc.x : 0;
        Append(b, "==== %s ====\n", reason);
        Append(b, "thread %" PRIu64 ", image 0x%016" PRIx64 "\n", tid, base);
        Append(b, "pc 0x%016" PRIx64 " = image+0x%" PRIx64 "\n", pc, pc - base);
        if (ctx)
        {
            Append(b, "lr 0x%016" PRIx64 " = image+0x%" PRIx64 "\n", ctx->lr.x, ctx->lr.x - base);
            Append(b, "esr 0x%08x (EC 0x%02x)  far 0x%016" PRIx64 "  error_desc 0x%x\n",
                   ctx->esr, ctx->esr >> 26, ctx->far.x, ctx->error_desc);
            if (guest_nx::InWindow(ctx->far.x))
                Append(b, "far is guest address (window+0x%09" PRIx64 "); committed %zu MB, mapped %zu MB, last result 0x%08x\n",
                       ctx->far.x - reinterpret_cast<uint64_t>(guest_nx::Base()), guest_nx::CommittedBytes() >> 20,
                       guest_nx::MappedBytes() >> 20, guest_nx::LastResult());
            MemoryInfo fm;
            u32 pageInfo = 0;
            if (R_SUCCEEDED(svcQueryMemory(&fm, &pageInfo, ctx->far.x)))
                Append(b, "far region 0x%016" PRIx64 "+0x%" PRIx64 " type 0x%x perm 0x%x\n",
                       fm.addr, fm.size, fm.type, fm.perm);
            for (int i = 0; i < 29; ++i)
                Append(b, "x%-2d 0x%016" PRIx64 "%s", i, ctx->cpu_gprs[i].x, (i % 3 == 2) ? "\n" : "  ");
            Append(b, "\nfp  0x%016" PRIx64 "  sp  0x%016" PRIx64 "\n", ctx->fp.x, ctx->sp.x);

            Append(b, "frame chain:\n");
            uint64_t frame = ctx->fp.x;
            for (int i = 0; i < 40 && (frame & 7) == 0 && QueryReadable(frame, 16, &mi); ++i)
            {
                const uint64_t next = reinterpret_cast<const uint64_t*>(frame)[0];
                const uint64_t ret = reinterpret_cast<const uint64_t*>(frame)[1];
                if (ret >= textLo && ret < textHi) Append(b, "  #%02d image+0x%" PRIx64 "\n", i, ret - base);
                else Append(b, "  #%02d 0x%016" PRIx64 "\n", i, ret);
                if (next <= frame) break;
                frame = next;
            }
            // Return addresses left on the stack (right after a bl/blr).
            uint64_t sp = ctx->sp.x & ~uint64_t(7);
            if (QueryReadable(sp, 8, &mi))
            {
                const uint64_t end = (mi.addr + mi.size < sp + 0x8000) ? mi.addr + mi.size : sp + 0x8000;
                Append(b, "stack scan:\n");
                int hits = 0;
                for (uint64_t p = sp; p + 8 <= end && hits < 40; p += 8)
                {
                    const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
                    if (v < textLo + 4 || v >= textHi || (v & 3)) continue;
                    const uint32_t prev = *reinterpret_cast<const uint32_t*>(v - 4);
                    if ((prev & 0xFC000000u) != 0x94000000u && (prev & 0xFFFFFC1Fu) != 0xD63F0000u) continue;
                    Append(b, "  sp+0x%05" PRIx64 "  image+0x%" PRIx64 "\n", p - sp, v - base);
                    ++hits;
                }
            }
        }
        Append(b, "faults handled before this one: %" PRIu64 "\n\n", g_faultsIn.load());
        WriteCrashFile(b.data, b.len);
        busy.clear();
    }
}

namespace switch_fault
{
    uint64_t Handled() { return g_faultsIn.load(std::memory_order_relaxed); }
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    const uint64_t far = ctx->far.x;
    // EC 0x24/0x25: data abort (lower/same EL); 0x20/0x21: instruction abort.
    const uint32_t ec = ctx->esr >> 26;
    if ((ec == 0x24 || ec == 0x25) && guest_nx::InWindow(far))
    {
        // The same address faulting over and over is not a missing mapping.
        static std::atomic<uint64_t> lastFar{ 0 };
        static std::atomic<uint32_t> repeats{ 0 };
        const uint32_t again = (lastFar.exchange(far) == far) ? repeats.fetch_add(1) + 1 : (repeats.store(0), 0u);
        if (again < 64 && guest_nx::FaultIn(far, true))
        {
            g_faultsIn.fetch_add(1, std::memory_order_relaxed);
            RexResumeFromException(ctx);
        }
        CrashLog(again >= 64 ? "guest address keeps faulting" : "guest memory could not be mapped", ctx);
        fatalThrow(MAKERESULT(Module_HomebrewLoader, 0x101));
    }
    CrashLog("PES 2017 crashed", ctx);
    fatalThrow(MAKERESULT(Module_HomebrewLoader, 0x102));
}
