# Extending an experiment with perf counters

The binaries deliberately do not call `perf_event_open` themselves. A counter
in the timed region changes the thing you are timing, and unprivileged
`perf_event_open` fails when `perf_event_paranoid` is 2 or 3 (common on WSL2
and locked-down VMs). Measure from outside first. Add an in-process counter
only when you need the count paired with one sample.

## From the shell

`scripts/with_perf.sh` is a thin wrapper:

```bash
scripts/with_perf.sh false-sharing
scripts/with_perf.sh memory --iters 10
scripts/with_perf.sh syscall --quick
```

Useful events, by experiment:

| Experiment | What to look at | What it should show |
|---|---|---|
| process | `task-clock`, `page-faults`, `cpu-migrations` | fork+exec faults more than fork+_exit (the new binary, not the COW parent pages) |
| thread | `context-switches` | about one switch per create/join, not per stack page |
| context | `context-switches`, `cpu-migrations` | pipe and the mutex ring should show switches on the order of the handoffs; migrations should collapse when you pin |
| contention | `cycles`, `instructions`, `LLC-load-misses` | instructions per op stay flat as threads grow; misses and cycles do not |
| false-sharing | `L1-dcache-load-misses` or `LLC-load-misses`, `cycles`, `instructions` | same instruction count, miss count jumps on the packed case |
| memory | `page-faults`, `dTLB-load-misses` | lazy mmap reports ~0 faults; first touch reports thousands; second touch reports ~0 |
| affinity | `cpu-migrations`, `context-switches` | spread pinning drives migrations toward 0 during the compute |
| syscall | `cycles` | vDSO `clock_gettime` should not show up as a syscall at all; use strace for that, perf for the cycle gap |

Exact event names depend on the kernel and `perf list`. If `cache-misses`
is `<not supported>`, try `L1-dcache-loads`, `L1-dcache-load-misses`,
`LLC-loads`, `LLC-load-misses`.

WSL2 often has no `perf` that matches the Microsoft kernel. That is a host
limitation, not a lab failure. `strace` still works:

```bash
scripts/with_strace.sh syscall --quick
STRACE_MODE=trace scripts/with_strace.sh process --iters 3
```

`strace -c` is how you prove a libc call did not enter the kernel. vDSO
`clock_gettime` will be absent or far below the iteration count.
`syscall(SYS_getpid)` will not.

Pin the whole process when you want perf's per-CPU view to be boring:

```bash
CPUSET=0,1 scripts/with_taskset.sh false-sharing
```

## Adding a counter inside one sample

Drop this next to the experiment you are extending. It is a starting point,
not a second benchmark harness. Keep it out of the default build so a machine
where `perf_event_open` returns `EPERM` still runs the suite.

```cpp
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static long perf_event_open(perf_event_attr* attr, pid_t pid, int cpu,
                            int group, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group, flags);
}

struct HwCounter {
    int fd = -1;
    explicit HwCounter(std::uint64_t config) {
        perf_event_attr attr{};
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_HARDWARE;
        attr.config = config;          // PERF_COUNT_HW_CACHE_MISSES, ...
        attr.disabled = 1;
        attr.exclude_kernel = 0;       // set 1 if you want userspace only
        attr.exclude_hv = 1;
        fd = static_cast<int>(perf_event_open(&attr, 0, -1, -1, 0));
        if (fd < 0) throw std::runtime_error("perf_event_open");
    }
    ~HwCounter() { if (fd >= 0) ::close(fd); }
    void reset_enable() {
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    std::uint64_t disable_read() {
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        std::uint64_t value = 0;
        if (::read(fd, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value)))
            throw std::runtime_error("read counter");
        return value;
    }
};
```

Use it around the existing timed body, not instead of it:

```cpp
HwCounter misses(PERF_COUNT_HW_CACHE_MISSES);
misses.reset_enable();
const double ns = lab::time_ns([&] { /* the operation */ });
const auto nmiss = misses.disable_read();
```

Report `nmiss / ops` beside the latency row. A false-sharing run should show
roughly one miss per contested RMW and a padded run should show a miss rate
near zero. If both are near zero, the threads ran on one core and the
experiment is not measuring coherence.

`perf_event_open` counts the calling thread by default (`pid == 0`). For a
two-thread test, open the counter inside each worker (or use `pid` of the
worker and `cpu == -1`) and sum. A counter opened on the main thread will not
see the workers' misses.

Do not sum a counter across a context switch and call it "the switch cost".
The switch itself is mostly kernel time; `exclude_kernel = 1` hides it.
For the context experiment, `perf stat`'s `context-switches` event is the
honest check that you forced as many switches as you think you did.
