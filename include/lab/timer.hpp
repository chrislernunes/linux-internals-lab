#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

namespace lab {

// steady_clock is monotonic. high_resolution_clock is not required to be,
// and on libstdc++ it is often just an alias — so we do not use it.
using Clock = std::chrono::steady_clock;

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

// Keep a value "used" so a timed loop cannot be deleted as dead code.
template <class T>
inline void black_box(const T& value) {
    asm volatile("" : : "r"(value) : "memory");
}

#if defined(__x86_64__) || defined(__i386__)

// lfence serializes earlier instructions before rdtsc so the timed region
// cannot leak out. rdtscp waits for earlier instructions, then we lfence
// so later work cannot leak in. These are TSC ticks of the invariant
// (constant-rate) timestamp counter, NOT retired core cycles.
inline std::uint64_t tsc_begin() {
    unsigned lo, hi;
    asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

inline std::uint64_t tsc_end() {
    unsigned lo, hi;
    asm volatile("rdtscp\n\tlfence" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

inline bool tsc_available() { return true; }

#else

inline std::uint64_t tsc_begin() { return now_ns(); }
inline std::uint64_t tsc_end() { return now_ns(); }
inline bool tsc_available() { return false; }

#endif

inline bool cpu_has_flag(const std::string& flag) {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    const std::string key = "flags";
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            return line.find(flag) != std::string::npos;
        }
    }
    return false;
}

struct TscClock {
    double ns_per_tick = 1.0;
    bool calibrated = false;

    void calibrate() {
        calibrated = false;
        if (!tsc_available()) {
            return;
        }
        // Without a constant-rate TSC, tick deltas are not wall time
        // (frequency scaling). Fall back to steady_clock rather than lie.
        if (!cpu_has_flag("constant_tsc") && !cpu_has_flag("nonstop_tsc")) {
            return;
        }

        // Two windows. TSC and steady_clock are both wall time, so a
        // scheduling hiccup during the sleep scales numerator and
        // denominator together and does not bias the frequency estimate.
        double acc = 0.0;
        int good = 0;
        for (int i = 0; i < 2; ++i) {
            tsc_begin();
            const auto t0 = Clock::now();
            const auto c0 = tsc_begin();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            const auto c1 = tsc_end();
            const auto t1 = Clock::now();
            const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
            const double ticks = static_cast<double>(c1 - c0);
            if (ns > 0.0 && ticks > 0.0) {
                acc += ns / ticks;
                ++good;
            }
        }
        if (good > 0) {
            ns_per_tick = acc / static_cast<double>(good);
            calibrated = true;
        }
    }

    double ticks_to_ns(std::uint64_t ticks) const {
        return static_cast<double>(ticks) * ns_per_tick;
    }
};

inline TscClock& tsc() {
    static TscClock clock;
    return clock;
}

// Time one invocation. TSC when we have a constant-rate counter, else
// steady_clock. The memory clobbers on the TSC reads stop the compiler
// from moving `fn` outside the timed window.
template <class F>
double time_ns(F&& fn) {
    if (tsc().calibrated) {
        const auto a = tsc_begin();
        fn();
        const auto b = tsc_end();
        return tsc().ticks_to_ns(b - a);
    }
    const auto a = now_ns();
    fn();
    const auto b = now_ns();
    return static_cast<double>(b - a);
}

}  // namespace lab
