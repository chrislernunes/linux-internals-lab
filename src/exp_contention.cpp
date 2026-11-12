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

// Test-and-test-and-set. The inner load avoids a locked RMW while the
// lock is held, which would bounce the line even harder. Unfair: a new
// arriver can steal the lock from a thread that has been spinning.
class TtasLock {
public:
    void lock() {
        for (;;) {
            if (!flag_.test_and_set(std::memory_order_acquire)) return;
            while (flag_.test(std::memory_order_relaxed)) lab::cpu_relax();
        }
    }
    void unlock() { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_{};
};

// FIFO spinlock. `next` and `serving` sit on different lines so the
// unlock (serving) does not invalidate the line arrivals are fetching on.
class TicketLock {
public:
    void lock() {
        const std::uint32_t ticket = next_.fetch_add(1, std::memory_order_relaxed);
        while (serving_.load(std::memory_order_acquire) != ticket) lab::cpu_relax();
    }
    void unlock() { serving_.fetch_add(1, std::memory_order_release); }

private:
    alignas(64) std::atomic<std::uint32_t> next_{0};
    alignas(64) std::atomic<std::uint32_t> serving_{0};
};

enum class Mode { Mutex, Spin, Ticket, Atomic };

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Mutex: return "std::mutex";
        case Mode::Spin: return "ttas-spin";
        case Mode::Ticket: return "ticket-spin";
        case Mode::Atomic: return "atomic fetch_add";
    }
    return "unknown";
}

// Force the non-atomic increment to actually hit memory inside the lock.
// Without this, a compiler that inlines the spinlock could keep `slot` in
// a register and delete the critical section.
void bump(std::uint64_t& slot) {
    slot += 1;
    asm volatile("" : "+m"(slot)::);
}

double run_once(Mode mode, int nthreads, int iters, int outside_spins) {
    alignas(64) std::mutex mu;
    alignas(64) std::uint64_t plain = 0;
    alignas(64) TtasLock spin;
    TicketLock ticket;
    alignas(64) std::atomic<std::uint64_t> atom{0};

    const int cpus = lab::online_cpus();
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::mutex done_mu;
    std::condition_variable done_cv;
    int finished = 0;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(nthreads));
    for (int t = 0; t < nthreads; ++t) {
        threads.emplace_back([&, t] {
            // Pin so "n threads" means n cores when we have them, and real
            // oversubscription when we don't. An unpinned run can look
            // uncontended just because the scheduler stacked everyone.
            lab::pin_this_thread(t % cpus);
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) lab::cpu_relax();

            for (int i = 0; i < iters; ++i) {
                for (int s = 0; s < outside_spins; ++s) lab::cpu_relax();
                switch (mode) {
                    case Mode::Mutex: {
                        std::lock_guard lock(mu);
                        bump(plain);
                        break;
                    }
                    case Mode::Spin: {
                        spin.lock();
                        bump(plain);
                        spin.unlock();
                        break;
                    }
                    case Mode::Ticket: {
                        ticket.lock();
                        bump(plain);
                        ticket.unlock();
                        break;
                    }
                    case Mode::Atomic:
                        atom.fetch_add(1, std::memory_order_relaxed);
                        break;
                }
            }
            lab::black_box(plain);
            lab::black_box(atom.load(std::memory_order_relaxed));

            std::lock_guard lock(done_mu);
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
    return ns;
}

}  // namespace

int exp_contention(const lab::Config& cfg) {
    lab::print_title("4. mutex vs spinlock vs atomic");
    const int samples = lab::pick_count(cfg.iters, cfg.quick ? 4 : 11);
    const int warmup = lab::pick_count(cfg.warmup, cfg.quick ? 1 : 2);
    const int iters = lab::inner_work(cfg, 20000, 2000);
    const auto threads = lab::thread_sweep(cfg);
    const Mode modes[] = {Mode::Atomic, Mode::Mutex, Mode::Spin, Mode::Ticket};

    std::cout << "  Threads are pinned round-robin onto online CPUs.\n"
              << "  Latency is what one thread experienced per op (queueing\n"
              << "  included). Throughput is the aggregate of all threads.\n"
              << "  Per-trial inner iterations: " << iters << ".\n";

    lab::AffinityGuard saved;

    for (Mode mode : modes) {
        for (int n : threads) {
            saved.restore();
            const auto data = lab::collect(warmup, samples, [&] {
                return run_once(mode, n, iters, 0);
            });
            const std::string label = std::string(mode_name(mode)) + " n=" + std::to_string(n) +
                                       " outside=0";
            lab::emit(cfg, "contention", label, data, static_cast<double>(iters), "op",
                      static_cast<double>(n));
        }
    }

    // Lower duty cycle: time outside the lock dominates, so the lock type
    // matters less and mutex stops losing to a pure atomic.
    {
        const int n = threads.back();
        const int outside = lab::inner_work(cfg, 128, 32);
        saved.restore();
        const auto data = lab::collect(warmup, samples, [&] {
            return run_once(Mode::Mutex, n, iters, outside);
        });
        lab::emit(cfg, "contention",
                  "std::mutex n=" + std::to_string(n) + " outside=" + std::to_string(outside),
                  data, static_cast<double>(iters), "op", static_cast<double>(n));
    }

    lab::print_why(
        "  An uncontended std::mutex is one userspace cmpxchg on the futex word.\n"
        "  It does not enter the kernel. n=1 should sit close to a spinlock,\n"
        "  plus the call into libpthread.\n"
        "  An empty critical section also rarely enters FUTEX_WAIT once n>1.\n"
        "  glibc's default mutex spins adaptively before sleeping, and a single\n"
        "  increment is shorter than that spin, so a hot mutex stays in\n"
        "  userspace and tracks the spinlock. What you still measure is\n"
        "  coherence on the lock word: throughput stops scaling with threads.\n"
        "  The path that actually sleeps is the mutex ring in the context\n"
        "  experiment, where the waiter blocks because the owner does real\n"
        "  work between handoffs. Do not quote an empty-lock number as\n"
        "  'the cost of a contended mutex' in production.\n"
        "  A TTAS spinlock is unfair and stays in userspace. It wins or ties\n"
        "  the mutex while the critical section is tiny and threads fit on the\n"
        "  cores. It becomes the wrong tool when the holder can be preempted\n"
        "  WHILE holding it: every other spinner burns a timeslice and the\n"
        "  waiter cannot run. A one-increment section almost never hits that.\n"
        "  Lengthen the section before you conclude anything about\n"
        "  oversubscription. A ticket lock is FIFO, so it does not barge, and\n"
        "  it bounces two cache lines (next and serving). Under contention it\n"
        "  is usually slower than TTAS. Fairness is not free.\n"
        "  atomic fetch_add is one locked instruction. The line is Modified on\n"
        "  one core at a time. Extra cores do not add throughput; they add\n"
        "  coherence delay. memory_order_relaxed does not remove the x86 lock\n"
        "  prefix.\n"
        "  outside=N is the duty cycle. Real locks protect a fraction of the\n"
        "  work. An empty lock overstates the lock and understates the program.");
    return 0;
}
