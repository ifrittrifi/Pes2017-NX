#include "audio.h"
#include "../log.h"

extern "C"
{
#include <switch.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

// audoutInitialize() already opens the default output (48 kHz, stereo, s16):
// opening it a second time fails, which left the game silent. Buffers are
// tracked here and reclaimed with one GetReleased call per pass instead of one
// ContainsAudioOutBuffer IPC per buffer per frame.
namespace
{
    constexpr uint32_t kPoolSize = 64;              // 64 x 5.33 ms, above the mixer's high water
    constexpr uint32_t kSamplesPerFrame = 256;
    constexpr uint32_t kFrameBytes = kSamplesPerFrame * 2 * sizeof(int16_t);
    constexpr uint32_t kBufferBytes = 0x1000;       // audout wants 0x1000-aligned storage and size

    alignas(0x1000) uint8_t g_storage[kPoolSize][kBufferBytes];
    AudioOutBuffer g_buffers[kPoolSize];
    AudioOutBuffer* g_free[kPoolSize];
    uint32_t g_freeCount = 0;
    uint32_t g_outstanding = 0;
    bool g_initialised = false;
    std::mutex g_lock;

    inline int16_t ToPcm16(float v)
    {
        v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<int16_t>(std::lrintf(v * 32767.0f));
    }

    // Caller holds g_lock.
    void Reclaim()
    {
        for (uint32_t guard = 0; guard < kPoolSize; ++guard)
        {
            AudioOutBuffer* released = nullptr;
            u32 count = 0;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || count == 0 || !released) break;
            if (g_freeCount < kPoolSize) g_free[g_freeCount++] = released;
            if (g_outstanding) --g_outstanding;
        }
    }
}

bool switch_audio::Initialise()
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_initialised) return true;
    Result rc = audoutInitialize();
    if (R_FAILED(rc))
    {
        LOGW("audio: audoutInitialize failed 0x%08X", rc);
        return false;
    }
    if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 || audoutGetPcmFormat() != PcmFormat_Int16)
    {
        LOGW("audio: unexpected output %u Hz %u ch format %d", audoutGetSampleRate(), audoutGetChannelCount(),
             int(audoutGetPcmFormat()));
        audoutExit();
        return false;
    }
    std::memset(g_storage, 0, sizeof(g_storage));
    for (uint32_t i = 0; i < kPoolSize; ++i)
    {
        g_buffers[i] = {};
        g_buffers[i].buffer = g_storage[i];
        g_buffers[i].buffer_size = kBufferBytes;
        g_free[i] = &g_buffers[i];
    }
    g_freeCount = kPoolSize;
    g_outstanding = 0;
    rc = audoutStartAudioOut();
    if (R_FAILED(rc))
    {
        LOGW("audio: audoutStartAudioOut failed 0x%08X", rc);
        audoutExit();
        return false;
    }
    g_initialised = true;
    return true;
}

uint32_t switch_audio::QueuedFrames()
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_initialised) return 0;
    Reclaim();
    return g_outstanding;
}

bool switch_audio::Submit(const float* input, uint32_t samples)
{
    if (!input || samples != kSamplesPerFrame) return false;
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_initialised) return false;
    if (!g_freeCount) Reclaim();
    if (!g_freeCount) return false;

    AudioOutBuffer* buffer = g_free[--g_freeCount];
    auto* output = static_cast<int16_t*>(buffer->buffer);
    for (uint32_t i = 0; i < samples; ++i)
    {
        // Guest order: FL, FR, FC, LFE, BL, BR. Folded down to stereo.
        const float* s = input + i * 6;
        const float c = 0.70710678f * s[2];
        output[i * 2 + 0] = ToPcm16(s[0] + c + 0.5f * s[4]);
        output[i * 2 + 1] = ToPcm16(s[1] + c + 0.5f * s[5]);
    }
    buffer->data_offset = 0;
    buffer->data_size = kFrameBytes;
    if (R_FAILED(audoutAppendAudioOutBuffer(buffer)))
    {
        g_free[g_freeCount++] = buffer;
        return false;
    }
    ++g_outstanding;
    return true;
}
