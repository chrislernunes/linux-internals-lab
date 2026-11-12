#include "experiments.hpp"

#include "lab/affinity.hpp"
#include "lab/report.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Dependency chain so the loop stays latency-bound and cannot be deleted.
// The return value is consumed by the caller.
__attribute__((noinline)) std::uint64_t chew(std::uint64_t seed, int n) {
    std::uint64_t x = seed | 1u;
    for (int i = 0; i < n; ++i) {
        x = x * 0x9E3779B185EBCA87ull + static_cast<std::uint64_t>(i);
        x ^= x >> 30;
    }
    return x;
}

enum class PinMode { Free, Spread, Stack };

double run_compute(int nthreads, int work, PinMode mode) {
    const int cpus = lab::online_cpus();
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::mutex done_mu;
    std::condition_variable done_cv;
    int finished = 0;
    std::uint64_t sink = 0;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(nthreads));
    for (int t = 0; t < nthreads; ++t) {
        threads.emplace_back([&, t] {
            if (mode == PinMode::Spread) lab::pin_this_thread(t % cpus);
            if (mode == PinMode::Stack) lab::pin_this_thread(0);
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) lab::cpu_relax();
            const std::uint64_t got = chew(0xC0FFEEULL + static_cast<unsigned>(t), work);
            std::lock_guard lock(done_mu);
            sink ^= got;
            if (++finished == nthreads) done_cv.notify_one();
        });
    }

    while (ready.load(std::memory_order_acquire) != nthreads) lab::cpu_relax();
    const double ns = lab::time_ns([&] {
        go.store(true, std::memory_order_release);
        std::unique_lock lock(done_mu);
        done_cv.wait(lock, [&] { return finished == nthreads; });
    });
    for (auto& th : threads) th.join();
    lab::black_box(sink);
    return ns;
}

// Userspace flag handoff. No yield and no syscall.
// Different cores: a coherence round trip.
// Same core: the waiter cannot run until the kernel preempts the spinner,
// so the number is a timeslice, not a cache miss.
double run_flag(int rounds, int cpu_a, int cpu_b) {
    alignas(64) std::atomic<int> flag{0};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::mutex done_mu;
    std::condition_variable done_cv;
    int finished = 0;

    std::thread other([&] {
        lab::pin_this_thread(cpu_b);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) lab::cpu_relax();
        for (int i = 0; i < rounds; ++i) {
            while (flag.load(std::memory_order_acquire) != 1) lab::cpu_relax();
            flag.store(0, std::memory_order_release);
        }
        std::lock_guard lock(done_mu);
        ++finished;
        done_cv.notify_one();
    });

    lab::pin_this_thread(cpu_a);
    ready.fetch_add(1, std::memory_order_release);
    // Yield, do not pause. On the same core a pause-spin starves the
    // partner that still has to publish `ready`.
    while (ready.load(std::memory_order_acquire) != 2) sched_yield();

    const double ns = lab::time_ns([&] {
        go.store(true, std::memory_order_release);
        for (int i = 0; i < rounds; ++i) {
            while (flag.load(std::memory_order_acquire) != 0) lab::cpu_relax();
            flag.store(1, std::memory_order_release);
        }
        // Only the partner bumps `finished`. The predicate covers the
        // case where it notifies before this wait starts.
        std::unique_lock lock(done_mu);
        done_cv.wait(lock, [&] { return finished == 1; });
    });
    other.join();
    return ns;
}

}  // namespace

int exp_affinity(const lab::Config& cfg) {
    lab::print_title("7. CPU affinity and pinning");
    const int samples = lab::pick_count(cfg.iters, cfg.quick ? 4 : 8);
    const int warmup = lab::pick_count(cfg.warmup, 1);
    const int work = lab::inner_work(cfg, 2000000, 200000);
    const int rounds = lab::inner_work(cfg, 12, 4);
    const int cpus = lab::online_cpus();
    const int nthreads = cfg.threads > 0 ? cfg.threads : cpus;

    std::cout << "  Compute: " << nthreads << " threads, " << work
              << " dependent multiplies each.\n"
              << "  Flag ping-pong: " << rounds
              << " round trips, no syscall and no yield.\n";

    lab::AffinityGuard saved;

    auto compute = [&](const char* name, PinMode mode) {
        saved.restore();
        const auto data = lab::collect(warmup, samples, [&] {
            return run_compute(nthreads, work, mode);
        });
        return lab::emit(cfg, "affinity", name, data, static_cast<double>(work), "iter",
                         static_cast<double>(nthreads));
    };

    const lab::Summary free_s = compute("compute unpinned", PinMode::Free);
    const lab::Summary spread_s = compute("compute pinned spread", PinMode::Spread);
    const lab::Summary stack_s = compute("compute all pinned to cpu 0", PinMode::Stack);
    if (spread_s.mean > 0.0) {
        std::cout << "\n  ratio mean(stacked) / mean(spread) = " << (stack_s.mean / spread_s.mean)
                  << "   (compute makespan; ~nthreads if the work is CPU-bound)\n";
        std::cout << "  ratio mean(unpinned) / mean(spread) = " << (free_s.mean / spread_s.mean)
                  << "\n";
    }

    lab::Summary same_s;
    lab::Summary diff_s;
    bool have_diff = false;
    {
        saved.restore();
        const auto data = lab::collect(warmup, samples, [&] { return run_flag(rounds, 0, 0); });
        same_s = lab::emit(cfg, "affinity", "flag ping-pong same core", data,
                           static_cast<double>(rounds), "round-trip");
    }
    if (cpus >= 2) {
        saved.restore();
        const auto data = lab::collect(warmup, samples, [&] { return run_flag(rounds, 0, 1); });
        diff_s = lab::emit(cfg, "affinity", "flag ping-pong different cores", data,
                           static_cast<double>(rounds), "round-trip");
        have_diff = true;
    } else {
        std::cout << "\n  skip: flag ping-pong on two cores (only one CPU online)\n";
    }
    if (have_diff && diff_s.mean > 0.0) {
        std::cout << "\n  ratio mean(same core) / mean(different cores) = "
                  << (same_s.mean / diff_s.mean) << "\n";
    }

    lab::print_why(
        "  sched_setaffinity changes one thread's mask. It does not pin the\n"
        "  process. A new thread inherits the creator's mask at clone time;\n"
        "  we pin inside the new thread so the measurement does not depend on\n"
        "  that race. Migration costs an L1/L2 reload and, if the runqueue was\n"
        "  on another CPU, a wakeup IPI. On a quiet machine independent compute\n"
        "  already spreads out, so unpinned and pinned-spread often match.\n"
        "  Stacking every thread on cpu 0 is the control: the same work becomes\n"
        "  a single-core run, and the makespan should scale with the thread count.\n"
        "  The flag ping-pong is the latency half, and it is easy to misread.\n"
        "  There is no yield. On two cores the flag line bounces Modified\n"
        "  between the two L1s: a coherence round trip, typically tens to a few\n"
        "  hundred nanoseconds, plus the pause loop. On one core the spinner\n"
        "  owns the CPU until the scheduler preempts it, so a 'round trip' is\n"
        "  on the order of a timeslice (hundreds of microseconds to milliseconds).\n"
        "  That gap is the reason a busy-wait handoff must be pinned to two\n"
        "  cores or it must yield. Pinning a communicating pair to ONE core is\n"
        "  the right call only when they block (pipe, futex), as in the context\n"
        "  experiment, where the kernel can switch them immediately.\n"
        "  WSL2 and other KVM guests add steal time. A same-core number with a\n"
        "  huge p99 is often the vCPU not being scheduled by the host, not the\n"
        "  guest CFS. Say that out loud if you quote it.");
    return 0;
}
