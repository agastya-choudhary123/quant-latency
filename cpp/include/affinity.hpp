// Jitter-reduction primitives for the hot path: pin a thread to a core and lock
// the process into RAM so page faults never stall a latency-critical loop.
//
// These are the real levers a low-latency system pulls to tighten p99/p99.9.
// They are genuinely portable only on Linux; on macOS core pinning is advisory
// (Apple's scheduler ignores hard affinity on Apple Silicon) so we expose the
// same API and report honestly whether it took effect, rather than pretending.
#pragma once

#if defined(__linux__)
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#elif defined(__APPLE__)
#include <sys/mman.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread.h>
#endif

namespace fa {

// Pin the calling thread to `core`. Returns true only if the OS enforces it.
inline bool pin_to_core(int core) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(__APPLE__)
    // macOS only supports affinity *tags* (a hint that threads sharing a tag
    // want to share an L2), not hard pinning. We set it so the intent is real,
    // but report false because the scheduler may still migrate us.
    thread_affinity_policy_data_t pol = { core + 1 };
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_AFFINITY_POLICY, (thread_policy_t)&pol, 1);
    return false;
#else
    (void)core;
    return false;
#endif
}

// Lock all current + future pages into RAM (no page faults on the hot path).
inline bool lock_memory() {
#if defined(__linux__) || defined(__APPLE__)
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#else
    return false;
#endif
}

} // namespace fa
