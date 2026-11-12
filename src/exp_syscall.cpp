#include "experiments.hpp"

#include "lab/report.hpp"
#include "lab/timer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

__attribute__((noinline)) pid_t call_getpid() { return ::getpid(); }

__attribute__((noinline)) uid_t call_getuid() { return ::getuid(); }

__attribute__((noinline)) int call_clock(clockid_t id, timespec* ts) {
    return ::clock_gettime(id, ts);
}

struct Means {
    double getpid_libc = 0;
    double getpid_raw = 0;
    double getuid_libc = 0;
    double getuid_raw = 0;
    double clock_libc = 0;
    double clock_raw = 0;
    double clock_raw_clock = 0;
    double write_libc = 0;
};

}  // namespace

int exp_syscall(const lab::Config& cfg) {
    lab::print_title("8. system-call overhead");
    const int samples = lab::pick_count(cfg.iters, cfg.quick ? 6 : 30);
    const int warmup = lab::pick_count(cfg.warmup, cfg.quick ? 1 : 5);
    const int batch = lab::inner_work(cfg, 1000, 100);

    std::cout << "  Each sample times " << batch
              << " calls. Percentiles are percentiles of the batch mean,\n"
              << "  which is the right tool for a ~20-700 ns operation and the\n"
              << "  wrong tool for a tail-latency SLO. Single-shot tails need a\n"
              << "  histogram of individual rdtsc deltas; the batch mean hides them.\n";

    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull < 0) {
        std::cerr << "open /dev/null: " << std::strerror(errno) << "\n";
        return 1;
    }

    Means m;

    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                pid_t sink = 0;
                for (int i = 0; i < batch; ++i) sink = call_getpid();
                lab::black_box(sink);
            });
        });
        m.getpid_libc =
            lab::emit(cfg, "syscall", "libc getpid", data, batch, "call").mean;
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                long sink = 0;
                for (int i = 0; i < batch; ++i) sink = ::syscall(SYS_getpid);
                lab::black_box(sink);
            });
        });
        m.getpid_raw =
            lab::emit(cfg, "syscall", "syscall(SYS_getpid)", data, batch, "call").mean;
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                uid_t sink = 0;
                for (int i = 0; i < batch; ++i) sink = call_getuid();
                lab::black_box(sink);
            });
        });
        m.getuid_libc =
            lab::emit(cfg, "syscall", "libc getuid", data, batch, "call").mean;
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                long sink = 0;
                for (int i = 0; i < batch; ++i) sink = ::syscall(SYS_getuid);
                lab::black_box(sink);
            });
        });
        m.getuid_raw =
            lab::emit(cfg, "syscall", "syscall(SYS_getuid)", data, batch, "call").mean;
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                timespec ts{};
                int sink = 0;
                for (int i = 0; i < batch; ++i) sink |= call_clock(CLOCK_MONOTONIC, &ts);
                lab::black_box(sink);
                lab::black_box(ts.tv_nsec);
            });
        });
        m.clock_libc = lab::emit(cfg, "syscall", "libc clock_gettime MONOTONIC", data, batch,
                                 "call")
                           .mean;
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                timespec ts{};
                long sink = 0;
                for (int i = 0; i < batch; ++i) {
                    sink |= ::syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
                }
                lab::black_box(sink);
                lab::black_box(ts.tv_nsec);
            });
        });
        m.clock_raw = lab::emit(cfg, "syscall", "syscall(SYS_clock_gettime) MONOTONIC", data,
                                batch, "call")
                          .mean;
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                timespec ts{};
                int sink = 0;
                for (int i = 0; i < batch; ++i) sink |= call_clock(CLOCK_MONOTONIC_RAW, &ts);
                lab::black_box(sink);
                lab::black_box(ts.tv_nsec);
            });
        });
        m.clock_raw_clock =
            lab::emit(cfg, "syscall", "libc clock_gettime MONOTONIC_RAW", data, batch, "call")
                .mean;
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                char byte = 'x';
                int sink = 0;
                for (int i = 0; i < batch; ++i) {
                    const ssize_t n = ::write(devnull, &byte, 1);
                    sink |= static_cast<int>(n);
                }
                lab::black_box(sink);
            });
        });
        m.write_libc =
            lab::emit(cfg, "syscall", "libc write 1B to /dev/null", data, batch, "call").mean;
    }
    {
        const int yield_batch = std::max(1, batch / 5);
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                for (int i = 0; i < yield_batch; ++i) ::sched_yield();
            });
        });
        lab::emit(cfg, "syscall", "sched_yield alone (not an empty syscall)", data, yield_batch,
                  "call");
    }

    ::close(devnull);

    std::cout << "\nobservations on this run:\n";
    if (m.getpid_raw > 0.0 && m.getpid_libc < 0.35 * m.getpid_raw) {
        std::cout << "  libc getpid is much cheaper than syscall(SYS_getpid).\n"
                     "  glibc caches the pid in the TCB after the first call, so\n"
                     "  this loop never enters the kernel. strace -c will not show\n"
                     "  a matching getpid count. That cache is the bug people hit\n"
                     "  after a raw clone that does not update the TCB.\n";
    } else {
        std::cout << "  libc getpid was not clearly cheaper than the raw syscall.\n"
                     "  Either this libc does not cache it, or the timer noise\n"
                     "  swallowed the difference. Check with strace -c.\n";
    }
    if (m.getuid_raw > 0.0 && m.getuid_libc > 0.6 * m.getuid_raw) {
        std::cout << "  libc getuid tracked the raw syscall. It is not cached the\n"
                     "  way getpid is, which is the useful contrast.\n";
    }
    if (m.clock_raw > 0.0 && m.clock_libc < 0.5 * m.clock_raw) {
        std::cout << "  libc clock_gettime(CLOCK_MONOTONIC) is much cheaper than\n"
                     "  syscall(SYS_clock_gettime). It is being served by the vDSO:\n"
                     "  a userspace function mapped by the kernel, reading a shared\n"
                     "  data page, with no privilege transition.\n";
    } else {
        std::cout << "  libc clock_gettime(CLOCK_MONOTONIC) was not clearly in the\n"
                     "  vDSO on this run (or noise hid it). CLOCK_MONOTONIC_RAW mean\n"
                     "  was " << m.clock_raw_clock
                  << " ns; RAW is the clock most likely to still be a syscall\n"
                     "  when MONOTONIC is not.\n";
    }
    std::cout << "  write(/dev/null) does an fd lookup and a file_operations\n"
                 "  call on top of the syscall instruction. It is not 'an empty\n"
                 "  syscall' and it does not have to match getpid. If it is far\n"
                 "  more expensive, the cost is the write path. Measured mean "
              << m.write_libc << " ns.\n";

    lab::print_why(
        "  A direct syscall pays for the privilege boundary: the syscall\n"
        "  instruction, the swap onto the kernel stack, whatever speculation\n"
        "  mitigation this CPU still uses, and the return. On a modern x86\n"
        "  without a full KPTI CR3 reload that can be under 100 ns. It is not\n"
        "  20 ns, and it is not a microsecond either, until the syscall does\n"
        "  real work (write, futex, mmap).\n"
        "  libc is allowed to skip the trip. clock_gettime(CLOCK_MONOTONIC) is\n"
        "  typically the vDSO: a userspace function the kernel maps into every\n"
        "  process, reading a shared data page. See [vdso] in /proc/self/maps.\n"
        "  getpid() is the example people still get wrong. glibc cached it in\n"
        "  the TCB for years and REMOVED the cache in 2.25 (2017) because the\n"
        "  cache was stale across some clone/pid-namespace cases. On a current\n"
        "  glibc, libc getpid and syscall(SYS_getpid) should be in the same\n"
        "  ballpark, and strace -c should show both. If you ever see getpid at\n"
        "  a couple of nanoseconds, you are on an old cache, or the compiler\n"
        "  deleted the call.\n"
        "  sched_yield is a syscall that may also call schedule(). Alone on a\n"
        "  quiet CPU it often finds nobody else runnable and returns, but it\n"
        "  is not an empty getpid. Do not average it into 'syscall cost'.\n"
        "  Confirm with:\n"
        "    strace -c ./build/linuxlab syscall --quick\n"
        "  vDSO clock_gettime will be missing or far below the iteration count.\n"
        "  write and syscall(SYS_getpid) will not.");
    return 0;
}
