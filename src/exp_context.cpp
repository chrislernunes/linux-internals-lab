#include "experiments.hpp"

#include "lab/affinity.hpp"
#include "lab/report.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct Fd {
    int fd = -1;
    explicit Fd(int f = -1) : fd(f) {}
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
};

void write_byte(int fd, char c) {
    for (;;) {
        const ssize_t n = ::write(fd, &c, 1);
        if (n == 1) return;
        if (n < 0 && errno == EINTR) continue;
        throw std::runtime_error("pipe write failed");
    }
}

char read_byte(int fd) {
    char c = 0;
    for (;;) {
        const ssize_t n = ::read(fd, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno == EINTR) continue;
        throw std::runtime_error("pipe read failed");
    }
}

struct PipePair {
    Fd read_end;
    Fd write_end;
    static PipePair make() {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) throw std::runtime_error("pipe() failed");
        return PipePair{Fd{fds[0]}, Fd{fds[1]}};
    }
};

// Main blocks in read(), so it does not steal the core the way a spin would.
// `cpu_a` / `cpu_b` < 0 means "do not pin".
std::vector<double> pipe_pingpong(int rounds, int samples, int warmup, int cpu_a, int cpu_b) {
    PipePair a2b = PipePair::make();
    PipePair b2a = PipePair::make();
    std::atomic<int> ready{0};

    std::thread worker([&] {
        if (cpu_b >= 0) lab::pin_this_thread(cpu_b);
        ready.store(1, std::memory_order_release);
        for (;;) {
            const char c = read_byte(a2b.read_end.fd);
            if (c == 'Q') break;
            write_byte(b2a.write_end.fd, c);
        }
    });

    struct Stop {
        std::thread& worker;
        int write_fd;
        bool armed = true;
        ~Stop() {
            if (!armed) return;
            try {
                write_byte(write_fd, 'Q');
            } catch (...) {
            }
            if (worker.joinable()) worker.join();
        }
    } stop{worker, a2b.write_end.fd, true};

    while (ready.load(std::memory_order_acquire) == 0) lab::cpu_relax();
    if (cpu_a >= 0) lab::pin_this_thread(cpu_a);

    auto once = [&] {
        char c = 'x';
        for (int i = 0; i < rounds; ++i) {
            write_byte(a2b.write_end.fd, c);
            c = read_byte(b2a.read_end.fd);
        }
        lab::black_box(c);
    };
    for (int i = 0; i < warmup; ++i) once();

    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) out.push_back(lab::time_ns(once));

    stop.armed = false;
    write_byte(a2b.write_end.fd, 'Q');
    worker.join();
    return out;
}

// Main sleeps on a condition variable so the two yielders own the CPU.
// Sample is the wall time until both have finished `yields` calls.
std::vector<double> yield_pair(int yields, int samples, int warmup) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(samples));

    for (int trial = 0; trial < warmup + samples; ++trial) {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::mutex done_mu;
        std::condition_variable done_cv;
        int finished = 0;

        auto body = [&] {
            lab::pin_this_thread(0);
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) lab::cpu_relax();
            for (int i = 0; i < yields; ++i) sched_yield();
            std::lock_guard lock(done_mu);
            if (++finished == 2) done_cv.notify_one();
        };

        std::thread a(body);
        std::thread b(body);
        while (ready.load(std::memory_order_acquire) != 2) lab::cpu_relax();

        const double ns = lab::time_ns([&] {
            go.store(true, std::memory_order_release);
            std::unique_lock lock(done_mu);
            done_cv.wait(lock, [&] { return finished == 2; });
        });
        a.join();
        b.join();
        if (trial >= warmup) out.push_back(ns);
    }
    return out;
}

// One hop hands a std::mutex/condvar token to the next thread. That is one
// futex wake. Main is asleep for the duration (see yield_pair).
std::vector<double> mutex_ring(int nthreads, int hops, int samples, int warmup, bool pin_spread) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(samples));
    const int cpus = lab::online_cpus();

    for (int trial = 0; trial < warmup + samples; ++trial) {
        std::mutex mu;
        std::vector<std::condition_variable> cvs(static_cast<std::size_t>(nthreads));
        int turn = 0;
        bool stop = false;
        int done_hops = 0;

        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::mutex done_mu;
        std::condition_variable done_cv;
        int finished = 0;

        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(nthreads));
        for (int i = 0; i < nthreads; ++i) {
            threads.emplace_back([&, i] {
                if (pin_spread) lab::pin_this_thread(i % cpus);
                else lab::pin_this_thread(0);
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) lab::cpu_relax();

                std::unique_lock lock(mu);
                while (!stop) {
                    cvs[static_cast<std::size_t>(i)].wait(
                        lock, [&] { return turn == i || stop; });
                    if (stop) break;
                    ++done_hops;
                    if (done_hops >= hops) {
                        stop = true;
                        turn = -1;
                        lock.unlock();
                        for (auto& cv : cvs) cv.notify_all();
                        break;
                    }
                    turn = (i + 1) % nthreads;
                    const int next = turn;
                    lock.unlock();
                    cvs[static_cast<std::size_t>(next)].notify_one();
                    lock.lock();
                }

                std::lock_guard done_lock(done_mu);
                if (++finished == nthreads) done_cv.notify_one();
            });
        }

        while (ready.load(std::memory_order_acquire) != nthreads) lab::cpu_relax();
        const double ns = lab::time_ns([&] {
            go.store(true, std::memory_order_release);
            std::unique_lock lock(done_mu);
            done_cv.wait(lock, [&] { return finished == nthreads; });
        });
        for (auto& t : threads) t.join();
        if (trial >= warmup) out.push_back(ns);
    }
    return out;
}

}  // namespace

int exp_context(const lab::Config& cfg) {
    lab::print_title("3. context-switch overhead");
    const int samples = lab::pick_count(cfg.iters, cfg.quick ? 4 : 20);
    const int warmup = lab::pick_count(cfg.warmup, cfg.quick ? 1 : 3);
    const int rounds = lab::inner_work(cfg, 300, 40);
    const int yields = lab::inner_work(cfg, 2000, 200);
    const int hops = lab::inner_work(cfg, 800, 80);
    const int cpus = lab::online_cpus();

    std::cout << "  Pipe samples time " << rounds
              << " round trips. A round trip is two switches plus four\n"
              << "  syscalls (write/read/write/read). The one-way figure is half\n"
              << "  of that: an upper bound on the switch, not a pure switch_to().\n"
              << "  Yield and mutex samples are batched the same way, so their\n"
              << "  percentiles are percentiles of batch means.\n";

    lab::AffinityGuard saved;

    auto run_pipe = [&](const std::string& name, int cpu_a, int cpu_b) {
        saved.restore();
        const auto batch = pipe_pingpong(rounds, samples, warmup, cpu_a, cpu_b);
        lab::emit(cfg, "context", name + " round-trip", batch, static_cast<double>(rounds),
                  "round-trip");
        lab::emit(cfg, "context", name + " one-way (half)", batch,
                  static_cast<double>(rounds) * 2.0, "one-way");
        saved.restore();
    };

    run_pipe("pipe unpinned", -1, -1);
    run_pipe("pipe same core", 0, 0);
    if (cpus >= 2) {
        run_pipe("pipe different cores", 0, 1);
    } else {
        std::cout << "\n  skip: pipe different cores (only one CPU online)\n";
    }

    {
        saved.restore();
        const auto batch = yield_pair(yields, samples, warmup);
        // Each of the two threads executed `yields` yields in this wall time.
        // Report the cost of one thread's yield (wall / yields). Throughput
        // counts both threads' yields (scale 2).
        lab::emit(cfg, "context", "sched_yield pair both pinned to cpu 0", batch,
                  static_cast<double>(yields), "yield", 2.0);
    }

    auto run_ring = [&](int n, bool spread, const std::string& name) {
        saved.restore();
        const auto batch = mutex_ring(n, hops, samples, warmup, spread);
        lab::emit(cfg, "context", name, batch, static_cast<double>(hops), "handoff");
    };

    for (int n : lab::thread_sweep(cfg)) {
        if (n < 2) continue;
        run_ring(n, false, "mutex ring n=" + std::to_string(n) + " all on cpu 0");
        if (cpus >= 2) {
            run_ring(n, true, "mutex ring n=" + std::to_string(n) + " pinned spread");
        }
    }

    lab::print_why(
        "  Threads of one process share an mm_struct, so switch_mm() is skipped\n"
        "  and a thread switch does not flush the user TLB the way switching to\n"
        "  another process can. The kernel still runs schedule(): runqueue lock,\n"
        "  switch_to() register save, and a reload of the next stack.\n"
        "  The pipe cannot avoid the kernel. Each direction is a blocking read,\n"
        "  so the reader is dequeued and the peer is woken. Same-core pinning\n"
        "  makes that wakeup local: no IPI, and the pipe buffer's cache line\n"
        "  stays warm. It is often the LOWER latency, not the higher one.\n"
        "  Different-core pinning pays an IPI plus a coherence miss so the peer\n"
        "  can run truly concurrently — which a ping-pong cannot use, because\n"
        "  each side is blocked. Cross-core wins for independent work (see the\n"
        "  affinity experiment), not for a single request-response handoff.\n"
        "  sched_yield() places the caller at the tail of its runqueue. With one\n"
        "  partner pinned to the same CPU, a yield is a voluntary switch rather\n"
        "  than a cheap return. The waiter (this process's main thread) sleeps\n"
        "  on a futex so it does not steal that CPU.\n"
        "  The mutex ring is the path inside std::mutex: the waiter blocks in\n"
        "  futex(WAIT), the owner calls futex(WAKE). The predicate on `turn` is\n"
        "  the condition; the notify is only a hint, so a notify that lands\n"
        "  before the next thread has waited is not a lost wakeup. More threads\n"
        "  on one CPU lengthen the runqueue. Spreading them trades that queue\n"
        "  for cross-core wakeups. These numbers include the mechanism that\n"
        "  forced the switch. A production switch is never a bare switch_to().");
    return 0;
}
