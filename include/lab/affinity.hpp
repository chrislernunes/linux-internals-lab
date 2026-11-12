#pragma once

#include "lab/config.hpp"
#include "lab/timer.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace lab {

inline int online_cpus() {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : static_cast<int>(n);
}

inline long page_size() {
    const long n = sysconf(_SC_PAGESIZE);
    return n < 1 ? 4096L : n;
}

inline std::string cpu_model() {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    const std::string key = "model name";
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            const auto pos = line.find(':');
            if (pos == std::string::npos) {
                continue;
            }
            std::string value = line.substr(pos + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            return value;
        }
    }
    return "unknown";
}

// Linux affinity is a per-thread attribute. sched_setaffinity(0, ...)
// changes the calling thread only. A new pthread inherits the creator's
// mask at pthread_create time.
inline bool pin_thread(pthread_t thread, int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(thread, sizeof(set), &set) == 0;
}

inline bool pin_this_thread(int cpu) {
    return pin_thread(pthread_self(), cpu);
}

inline int current_cpu() {
    const int cpu = sched_getcpu();
    return cpu;
}

// Saves and restores the calling thread's affinity mask. Worker threads
// are joined before an experiment returns, so their masks die with them.
class AffinityGuard {
public:
    AffinityGuard() {
        CPU_ZERO(&saved_);
        ok_ = sched_getaffinity(0, sizeof(saved_), &saved_) == 0;
    }

    bool ok() const { return ok_; }

    void restore() const {
        if (ok_) {
            sched_setaffinity(0, sizeof(saved_), &saved_);
        }
    }

    ~AffinityGuard() { restore(); }

    AffinityGuard(const AffinityGuard&) = delete;
    AffinityGuard& operator=(const AffinityGuard&) = delete;

private:
    cpu_set_t saved_{};
    bool ok_ = false;
};

// 1, ncpu, 2*ncpu — unless the user forced --threads.
inline std::vector<int> thread_sweep(const Config& cfg) {
    if (cfg.threads > 0) {
        return {cfg.threads};
    }
    const int n = online_cpus();
    std::vector<int> out;
    out.push_back(1);
    if (n != 1) {
        out.push_back(n);
    }
    const int over = n * 2;
    if (over != n && over != 1) {
        out.push_back(over);
    }
    return out;
}

// Reusable N-thread barrier that does not allocate and does not sleep.
// Last arriver publishes the next generation with release; waiters spin
// on that generation with acquire. Correct as long as each generation's
// waiters all enter before the following generation can complete (true
// for our benchmarks: every thread calls wait() once per phase).
class SpinBarrier {
public:
    explicit SpinBarrier(int n) : n_(n) {}

    void wait() {
        const int gen = generation_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == n_) {
            arrived_.store(0, std::memory_order_relaxed);
            generation_.fetch_add(1, std::memory_order_release);
        } else {
            while (generation_.load(std::memory_order_acquire) == gen) {
                cpu_relax();
            }
        }
    }

private:
    int n_;
    std::atomic<int> arrived_{0};
    std::atomic<int> generation_{0};
};

inline void print_environment() {
    std::cout << "linux-internals-lab\n";
    std::cout << "cpu: " << cpu_model() << "\n";
    std::cout << "cpus online: " << online_cpus() << "\n";
    std::cout << "page size: " << page_size() << " bytes\n";
    if (tsc().calibrated) {
        const double mhz = 1e3 / tsc().ns_per_tick;
        std::cout << "timer: invariant TSC, " << tsc().ns_per_tick
                  << " ns/tick (~" << mhz << " MHz TSC rate)\n";
        std::cout << "  TSC ticks are wall time, not retired core cycles.\n";
    } else {
        std::cout << "timer: std::chrono::steady_clock "
                     "(no constant_tsc, or not x86)\n";
    }
    std::cout << std::flush;
}

}  // namespace lab
