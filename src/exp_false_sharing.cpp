#include "experiments.hpp"

#include "lab/affinity.hpp"
#include "lab/report.hpp"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Packed {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};
static_assert(sizeof(Packed) <= 64, "packed counters must share a cache line");
static_assert(offsetof(Packed, b) - offsetof(Packed, a) < 64, "a and b separated");

struct alignas(64) Line64 {
    std::atomic<std::uint64_t> v{0};
};
static_assert(sizeof(Line64) == 64, "one counter, one 64B line");

struct alignas(128) Line128 {
    std::atomic<std::uint64_t> v{0};
};
static_assert(sizeof(Line128) == 128, "padding past the adjacent-line prefetcher");

double run_pair(std::atomic<std::uint64_t>& left,
                std::atomic<std::uint64_t>& right,
                int reps,
                int cpu_left,
                int cpu_right) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::mutex done_mu;
    std::condition_variable done_cv;
    int finished = 0;

    auto work = [&](std::atomic<std::uint64_t>& cell, int cpu) {
        lab::pin_this_thread(cpu);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) lab::cpu_relax();
        for (int i = 0; i < reps; ++i) {
            cell.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard lock(done_mu);
        if (++finished == 2) done_cv.notify_one();
    };

    std::thread a([&] { work(left, cpu_left); });
    std::thread b([&] { work(right, cpu_right); });
    while (ready.load(std::memory_order_acquire) != 2) lab::cpu_relax();

    const double ns = lab::time_ns([&] {
        go.store(true, std::memory_order_release);
        std::unique_lock lock(done_mu);
        done_cv.wait(lock, [&] { return finished == 2; });
    });
    a.join();
    b.join();
    return ns;
}

}  // namespace

int exp_false_sharing(const lab::Config& cfg) {
    lab::print_title("5. false sharing");
    const int samples = lab::pick_count(cfg.iters, cfg.quick ? 4 : 9);
    const int warmup = lab::pick_count(cfg.warmup, cfg.quick ? 1 : 2);
    const int reps = lab::inner_work(cfg, 300000, 30000);
    const int cpus = lab::online_cpus();
    const int cpu_b = cpus >= 2 ? 1 : 0;

    std::cout << "  Two threads, each doing " << reps
              << " relaxed fetch_adds on its own counter.\n"
              << "  Pinned to cpu 0 and cpu " << cpu_b
              << ". Same instructions, different addresses.\n";
    if (cpus < 2) {
        std::cout << "  Only one CPU is online. Both threads share it, so the\n"
                     "  coherence penalty will be partly hidden by time-slicing.\n";
    }

    lab::AffinityGuard saved;

    alignas(128) Packed packed;
    alignas(128) Line64 lines64[2];
    alignas(128) Line128 lines128[2];

    saved.restore();
    const auto shared = lab::collect(warmup, samples, [&] {
        return run_pair(packed.a, packed.b, reps, 0, cpu_b);
    });
    const lab::Summary shared_s =
        lab::emit(cfg, "false-sharing", "same line (packed atomics)", shared,
                  static_cast<double>(reps), "rmw", 2.0);

    saved.restore();
    const auto pad64 = lab::collect(warmup, samples, [&] {
        return run_pair(lines64[0].v, lines64[1].v, reps, 0, cpu_b);
    });
    const lab::Summary pad64_s =
        lab::emit(cfg, "false-sharing", "padded 64B", pad64, static_cast<double>(reps), "rmw",
                  2.0);

    saved.restore();
    const auto pad128 = lab::collect(warmup, samples, [&] {
        return run_pair(lines128[0].v, lines128[1].v, reps, 0, cpu_b);
    });
    const lab::Summary pad128_s =
        lab::emit(cfg, "false-sharing", "padded 128B", pad128, static_cast<double>(reps), "rmw",
                  2.0);

    if (pad64_s.mean > 0.0) {
        std::cout << "\n  ratio mean(same line) / mean(64B pad) = "
                  << (shared_s.mean / pad64_s.mean) << "\n";
    }
    if (pad128_s.mean > 0.0 && pad64_s.mean > 0.0) {
        std::cout << "  ratio mean(64B pad) / mean(128B pad) = "
                  << (pad64_s.mean / pad128_s.mean)
                  << "  (above ~1 suggests adjacent-line prefetch coupling)\n";
    }

    lab::print_why(
        "  A cache line (64 bytes on this x86) is the unit of coherence, not\n"
        "  the variable. Two cores cannot both hold the same line in Modified.\n"
        "  Each fetch_add needs the line Exclusive/Modified, which invalidates\n"
        "  the other core. That is a read-for-ownership (RFO) per increment,\n"
        "  even though the threads never touch each other's counter and even\n"
        "  though the memory order is relaxed. On x86 the RMW is a lock-prefixed\n"
        "  instruction regardless of memory_order_relaxed.\n"
        "  Padding to 64 bytes gives each core its own line, so the increment\n"
        "  hits in L1. Padding to 128 bytes also steps past Intel's adjacent-line\n"
        "  prefetcher, which otherwise pulls the neighbour and can re-couple two\n"
        "  'nicely padded' counters. If the 64B and 128B results match, the\n"
        "  prefetcher is not the bottleneck on this CPU today; the 64B result\n"
        "  is still the one to ship in a struct-of-counters.\n"
        "  Verify, do not trust the story alone:\n"
        "    perf stat -e cache-misses,LLC-loads,LLC-load-misses,cycles,instructions \\\n"
        "      taskset -c 0,1 ./build/linuxlab false-sharing\n"
        "  The same-line case should show a miss storm with almost the same\n"
        "  instruction count. If both look identical, the threads were not\n"
        "  actually running on two cores (check /proc/self/status Cpus_allowed\n"
        "  and that you have two online CPUs).");
    return 0;
}
