// Sampling CPU profiler for Horizon (after UnleashedRecomp-NX's
// os/switch/cpu_profiler_switch.cpp): a sampler thread pauses each registered
// thread every few milliseconds, reads its program counter and resumes it.
// Every 20 seconds it writes, per thread, its share of a core and its hottest
// code addresses to sdmc:/switch/PES2017/profile.txt, as offsets into the
// executable that pes17.map names (guest functions are sub_XXXXXXXX).
// Off with PES17_PROFILE=0.
#pragma once

namespace profiler_nx
{
    void RegisterCurrentThread(const char* name);
    void UnregisterCurrentThread();
    void Start();
}
