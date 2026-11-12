# linux-internals-lab

A C++20 lab that measures real Linux behavior — process and thread lifetime,
context switches, lock contention, cache-line sharing, page faults, affinity,
and the syscall path — and reports distributions, not a single average.

It is meant to be cloned, built on WSL2 Ubuntu or any recent Linux, and
talked through in an interview. Every number is something you can regenerate.
The interesting part is why the shape is what it is.

No kernel modules, no root, no special hardware. `perf` and `strace` are
optional; the suite runs without them.

## Layout

```
include/lab/     timing, percentiles, affinity, reporting
src/main.cpp     one binary, one subcommand per experiment
src/exp_*.cpp    the experiments
scripts/         perf, strace, taskset, the whole suite
docs/perf.md     how to add perf counters without lying to yourself
docs/example_output.md
```

One binary on purpose. The utilities are header-only so an experiment can be
read top to bottom, and so adding a ninth experiment is a new `.cpp` plus a
line in `main`, not a new framework.

```
linuxlab process | thread | context | contention
         false-sharing | memory | affinity | syscall
linuxlab all
linuxlab self-test
```

## Build

```bash
sudo apt install build-essential cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/linuxlab self-test
./build/linuxlab all --csv results/suite.csv
```

CMake options:

| option | default | |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | `Debug` disables optimisation. Do not time a Debug build. |
| `LAB_NATIVE_ARCH` | `ON` | `-march=native`. Turn off for a portable CI binary. |
| `LAB_ASAN` | `OFF` | AddressSanitizer and UBSan. Timings become meaningless. |
| `LAB_UBSAN` | `OFF` | UBSan alone. |
| `LAB_TSAN` | `OFF` | ThreadSanitizer. Cannot combine with ASan. |

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLAB_ASAN=ON -DLAB_NATIVE_ARCH=OFF
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=0 ./build-asan/linuxlab self-test
```

The process experiment `fork`s. ASan is fine with that because the child
`_exit`s or `exec`s, but leak detection at exit is noisy. TSan will run; do
not treat its timings as data.

Flags you will actually use:

```bash
./build/linuxlab false-sharing
./build/linuxlab contention --threads 4 --iters 20
./build/linuxlab all --quick          # smoke, not a result
./build/linuxlab syscall --csv results/syscall.csv
scripts/run_suite.sh                  # all + timestamped CSV
scripts/run_suite.sh --quick
```

`--iters` is the number of samples. `--warmup` iterations are run and thrown
away. `--quick` shrinks the inner loops (ops per sample), not the statistics
code. `--threads` replaces the sweep `1, ncpu, 2*ncpu` where a sweep exists.

CSV rows are always **nanoseconds per operation**, whatever unit the console
picked. Columns: `experiment,variant,samples,ops_per_sample,min_ns,p50_ns,p90_ns,p99_ns,max_ns,mean_ns,stddev_ns,throughput_per_s`.

## How the numbers are produced

- Timer is the invariant TSC (`lfence; rdtsc` / `rdtscp; lfence`), converted
  to nanoseconds with a short calibration against `steady_clock`. Those ticks
  are wall time, not retired core cycles. If `constant_tsc` is missing, the
  lab falls back to `steady_clock` and says so. `high_resolution_clock` is
  not used; it is not required to be monotonic.
- Warmup runs first and is not recorded.
- Percentiles are linear interpolation between ranks. p50 of `{1..10}` is 5.5.
  Standard deviation is the sample standard deviation (divide by n−1).
- `linuxlab self-test` checks that math. CI runs it.
- Short operations (syscall, atomic RMW) are batched. The percentile is then
  a percentile of batch means, which hides single-call tails. The banner says
  when that is happening. Long operations (fork, thread create, a full
  first-touch) are one operation per sample.
- Throughput for a parallel run is aggregate work divided by the makespan.
  The latency next to it is what one thread experienced per op. Both are
  labelled.
- Outliers are kept. When stddev exceeds the mean the row says so. Prefer
  p50/p90 in that case. See posix_spawn in the example output: the mean and
  the median disagree, and the median is the one that describes the path.
- Nothing is pinned unless the variant says it is. Pinning uses
  `pthread_setaffinity_np` on the worker, not on the process. Main sleeps on
  a condition variable while workers run, so a spinner on the main thread
  does not steal the core under test.

## WSL2

The suite was written for WSL2 Ubuntu and does not need a custom kernel.

- Absolute times are worse and noisier than bare metal. The host schedules
  the VM, so a large p99 on a same-core test is often steal time. Say that
  if you quote it.
- `sched_setaffinity` works. `mlock` works up to `RLIMIT_MEMLOCK` and is
  skipped, not failed, when it does not.
- `perf` often cannot be installed because the tools package must match the
  Microsoft kernel. `docs/perf.md` has the fallback. `strace` and `taskset`
  are ordinary apt packages.
- You will usually see one NUMA node and two to eight vCPUs. The cross-core
  variants skip themselves when only one CPU is online.

## Experiments

Each subcommand prints a `why:` block. The notes below are the longer form.
Real output from two of them is in [docs/example_output.md](docs/example_output.md).

### 1. `process` — fork, exec, posix_spawn

Three variants. Each sample is one create plus `waitpid`:

- `fork` + `_exit(0)`. No exec. The child does not return into C++.
- `fork` + `execl("/bin/true")` + wait.
- `posix_spawn("/bin/true")` + wait.

`fork` is `clone` without `CLONE_VM`. The kernel copies the `task_struct`,
the fd table, and the page tables. The pages stay copy-on-write, so a large
heap is not memcpy'd, but the page tables are, and that cost grows with the
parent. `execve` throws the address space away: load the ELF, map the
interpreter, flush the TLB. On a small process, exec is the dominant term and
fork+exec is several times fork alone.

`posix_spawn` is the API to actually use. Current glibc implements the common
case as `clone(CLONE_VM|CLONE_VFORK)`, so the child borrows the parent address
space until exec and the page tables are not copied. The parent is stopped for
that window, which is what makes the sharing safe. The win scales with the
parent's page tables. In this tiny process it can be a small gap. Allocate a
multi-GB buffer and rerun if you want it to be obvious.

Raw `clone` from C++ is not here. The child would share or drop glibc's TLS,
and that corrupts the parent. `strace -f -e clone,clone3,execve` is the way
to see the flags.

Do not quote a number from this binary as "the cost of fork" in a server that
has a 20 GB heap and 200 threads. Fork walks the page tables of the parent it
actually has.

### 2. `thread` — std::thread and pthread

One create + start + exit + join per sample. Only one extra thread is alive,
so this does not trip `RLIMIT_NPROC`.

- `std::thread`
- `pthread_create` / `pthread_join`, default stack
- the same with a 256 KiB stack

`pthread_create` is `clone` with `CLONE_VM | CLONE_FS | CLONE_FILES |
CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID`. The address space is shared, so
there is no page-table copy. Join waits on the futex the kernel writes at
exit (`CHILD_CLEARTID`).

The default stack is a virtual mapping, often 8 MiB, plus a guard page.
glibc `mmap`s it. An empty thread does not fault those pages. If the gap
between 8 MiB and 256 KiB were page faults, it would be hundreds of
microseconds — the memory experiment's regime. A gap of tens of microseconds
is the larger VMA, not the RSS. `std::thread` on libstdc++ is that pthread
path plus a small allocation for the callable, so it should track the
default-stack number.

### 3. `context` — forced switches

Three mechanisms, because "the context switch" is not a thing you can time
on its own. You always time the mechanism that forced it.

- **Pipe ping-pong.** Two threads, two pipes, a one-byte round trip. That is
  two wakeups and four syscalls. The tool prints the round trip and the half
  (an upper bound on one switch, not a pure `switch_to`). Unpinned, both on
  cpu 0, and on cpu 0 and cpu 1.
- **`sched_yield` pair**, both pinned to cpu 0. With a partner runnable, a
  yield is a voluntary `schedule()`, not a cheap userspace pause.
- **Mutex/condvar token ring** of n = 2, ncpu, 2·ncpu threads, either all on
  cpu 0 or spread. One hop is one `FUTEX_WAKE`. The predicate on `turn` is
  the condition; the notify is a hint, so a notify that lands early is not a
  lost wakeup.

Threads share an `mm_struct`, so `switch_mm` is skipped and a thread switch
does not flush the user TLB the way a process switch can. You still pay
`schedule()`: runqueue lock, `switch_to`, reload of the next stack.

On a blocking ping-pong, same-core is often the *lower* latency. The wakeup
is local, there is no IPI, and the pipe buffer stays in one L1. Cross-core
pays the IPI and a coherence miss so the peer can run concurrently — which a
ping-pong cannot use, because each side is asleep. Cross-core wins for
independent work, measured in `affinity`, not for request/response.

Main sleeps on a futex while the yielders run. A spinning main thread on the
same core would be a third runnable task and the number would be fiction.

### 4. `contention` — mutex, spin, ticket, atomic

Threads are pinned round-robin onto online CPUs. Each does N increments:

- `std::mutex` around a non-atomic counter
- a test-and-test-and-set spinlock (`atomic_flag`, with a `pause` loop)
- a ticket spinlock, `next` and `serving` on separate cache lines
- `atomic<uint64_t>::fetch_add(1, relaxed)`

n = 1, ncpu, 2·ncpu, plus one mutex row with work *outside* the lock so the
duty cycle drops.

An uncontended `std::mutex` is a userspace `cmpxchg` on the futex word. It
does not enter the kernel. An empty critical section also rarely enters
`FUTEX_WAIT` once n > 1: glibc spins adaptively before sleeping, and one
increment is shorter than that spin. The hot mutex stays in userspace and
tracks the spinlock. What does not scale is the cache line. Extra threads
do not add throughput.

The path that actually sleeps is the mutex ring in `context`, where the
waiter blocks. Do not quote an empty-lock number as the cost of a contended
mutex in a program whose critical section does real work.

TTAS is unfair and fine while the section is tiny. It is the wrong tool when
the holder can be preempted *while holding it*: every spinner then burns a
timeslice. A one-increment section almost never hits that. Lengthen the
section before you draw the oversubscription conclusion. The ticket lock is
FIFO, so it cannot barge, and it bounces two lines. Under contention it is
usually slower than TTAS. Fairness is not free.

`memory_order_relaxed` does not remove the x86 `lock` prefix. The line is
still Modified on one core at a time.

The non-atomic counter is bumped through a `"+m"` asm constraint. Without
that, a compiler that can see the spinlock could keep the counter in a
register and delete the critical section.

### 5. `false-sharing`

Two threads, pinned to two cores when possible, each doing a relaxed
`fetch_add` on its own counter.

- both atomics in one struct (same 64 B line)
- each on its own `alignas(64)` line
- each on its own `alignas(128)` line

Same instructions, different addresses. The packed case should be clearly
slower. 128 B padding is there because Intel's adjacent-line prefetcher can
re-couple two counters that are "only" 64 B apart. If 64 and 128 match, the
prefetcher is not your problem today; pad to 64 anyway.

Check it, do not trust the ratio alone:

```bash
scripts/with_perf.sh false-sharing
# or
perf stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses \
    taskset -c 0,1 ./build/linuxlab false-sharing
```

Instruction counts should match. Misses should not. If the two layouts match,
the threads were not on two cores.

### 6. `memory` — malloc, mmap, faults, mlock

- 64 B `operator new` / `delete` plus a one-byte touch, batched. After warmup
  this is the arena freelist, not a fault per call.
- `mmap` + `munmap` of an anonymous region, no touch. A VMA and little else.
- first write to every page of a fresh mapping, with `MADV_NOHUGEPAGE` so
  transparent huge pages do not turn the loop into a handful of 2 MiB faults.
- the same mapping written a second time.
- `mmap(MAP_POPULATE)`.
- large `operator new` + touch + delete. Above glibc's `M_MMAP_THRESHOLD`
  (default 128 KiB) this is its own mmap, unmapped on free. p50 can collapse
  toward the second-touch cost when the allocator reuses a mapping; the
  explicit mmap row is the clean fault measurement.
- `mlock`, if `RLIMIT_MEMLOCK` allows it. Otherwise a skip, with the limit
  printed.

The first touch is the fault handler: a frame, a PTE, a TLB invalidation.
The second touch is store bandwidth and TLB hits. The ratio is the result.
Do not divide by 4096 and call it "the" page-fault cost. Linux faults a
cluster of neighbouring pages on one fault (fault-around), so a stride loop
is not one full fault per page. WSL2 adds a host transition; the
microseconds move, the ratio does not.

### 7. `affinity`

Two questions.

**Throughput.** ncpu threads run a dependent multiply chain (so it cannot be
deleted or vectorized into irrelevance): unpinned, pinned one thread per
core, and all pinned to cpu 0. Stacked makespan should scale with the thread
count. Unpinned often matches spread on a quiet machine, which is the
scheduler doing its job. The gap is the interesting failure, not the match.

**Latency.** Two threads hand a flag back and forth with no yield and no
syscall. Across two cores this is a coherence round trip (the line goes
Modified in one L1, then the other). On one core the waiter cannot run until
the spinner is preempted, so a "round trip" becomes a timeslice. The ratio
is typically three or four orders of magnitude. That is why a busy-wait
handoff must be pinned to two cores or must yield, and why pinning a
*blocking* pair (the pipe, the mutex ring) to one core is a completely
different decision.

### 8. `syscall` — libc vs the instruction

Batched, because a single syscall is not much bigger than the timer:

- libc `getpid` vs `syscall(SYS_getpid)`
- libc `getuid` vs `syscall(SYS_getuid)`
- libc `clock_gettime(CLOCK_MONOTONIC)` vs `syscall(SYS_clock_gettime)`
- libc `clock_gettime(CLOCK_MONOTONIC_RAW)`
- `write` of one byte to `/dev/null`
- `sched_yield` alone, labelled as not an empty syscall

A direct syscall pays for the privilege boundary. On a modern x86 that is
tens to a few hundred nanoseconds when the kernel does almost nothing, and
more when it does (write, futex, mmap). It is not 20 ns.

libc is allowed to skip the trip. `clock_gettime(CLOCK_MONOTONIC)` is
typically the vDSO: code the kernel maps into every process, reading a shared
page. See `[vdso]` in `/proc/self/maps`. `CLOCK_MONOTONIC_RAW` is the clock
most likely to still enter the kernel when `MONOTONIC` does not.

`getpid` is the example that has gone stale. glibc cached it in the TCB for
years and **removed the cache in 2.25** because it returned the wrong pid
across some `clone` and pid-namespace cases. On a current glibc, libc
`getpid` and `syscall(SYS_getpid)` should match, and `strace -c` should show
both. A getpid at a couple of nanoseconds means an old libc or a compiler
that deleted the call. The lab prints which of the two it observed.

`write(/dev/null)` is a real syscall plus an fd lookup and `file_operations`.
It does not have to match getpid. If it is far more expensive, that cost is
the write path.

```bash
scripts/with_strace.sh syscall --quick
```

## perf, strace, taskset

```bash
scripts/with_perf.sh false-sharing
scripts/with_strace.sh syscall --quick
STRACE_MODE=trace scripts/with_strace.sh process --iters 3
CPUSET=0 scripts/with_taskset.sh contention --threads 4
CPUSET=0,1 scripts/with_taskset.sh context
```

`taskset` sets the mask of the whole process at start. The affinity and
context experiments then pin individual threads themselves; a process-wide
mask can only shrink the set they may use.

If `perf stat` fails with "permission denied", read
`/proc/sys/kernel/perf_event_paranoid`. Unprivileged counting usually needs
it at 1 or below (`sudo sysctl kernel.perf_event_paranoid=1`). WSL2 may not
have a working `perf` at all. Details, including a `perf_event_open` snippet
for adding a counter inside one sample, are in [docs/perf.md](docs/perf.md).

## Adding an experiment

1. `src/exp_foo.cpp` with `int exp_foo(const lab::Config&)`.
2. Declare it in `src/experiments.hpp`.
3. Dispatch it from `src/main.cpp`, including the `all` path.
4. Use `lab::collect` for warmup and samples, `lab::time_ns` around the
   operation, `lab::emit` to print and optionally append the CSV.
5. Print a `why:` that says what would *falsify* the result, not just what
   you hope it shows.
6. Keep the new file free of undefined behavior. The non-atomic counter in
   the contention test is the template for "the compiler can see this".

Inner loop sizes belong behind `lab::inner_work(cfg, normal, quick)` so
`--quick` stays a smoke test.

## What this is not

- Not a throughput suite for your production allocator. The regions are small
  on purpose.
- Not a claim about a tuned bare-metal box. Disable turbo and isolate CPUs
  (`isolcpus`, `nohz_full`) if you want numbers you can publish; the lab will
  still be right about the shape.
- Not a microbenchmark of `std::chrono` call overhead. Short ops are batched
  so that overhead is amortized, and the batching is disclosed.

## Next experiments worth adding

- TLB shootdown: many threads, then `munmap` a mapping they have all touched.
- `FUTEX_WAKE` 1 vs `FUTEX_WAKE` N (thundering herd) against the token ring.
- `epoll_wait` vs `io_uring` NOP, to separate wakeup from the ring protocol.
- page-cache read vs `O_DIRECT`, same file, cold vs hot cache.
- transparent huge pages on vs `MADV_NOHUGEPAGE`, now that the fault test
  forces the base-page path.
- a cyclictest-style wakeup latency histogram under load.
- pipe bandwidth (`read`/`write` of 64 B vs 64 KiB) next to the 1-byte
  ping-pong, so latency and bandwidth are not confused.
- NUMA first-touch, on a machine that actually has two nodes. WSL2 usually
  does not.

## License

MIT. See [LICENSE](LICENSE).
