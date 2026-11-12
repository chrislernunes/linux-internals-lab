# Example output

Two real runs, not synthetic numbers. The host was a 2-vCPU KVM guest
(Intel Xeon Platinum 8481C @ 2.70 GHz, invariant TSC ~2.70 GHz, 4 KiB pages).
This is the same class of machine as a small cloud VM or a constrained WSL2
session: absolute microseconds will move, the shape should not.

Full logs:

- [example_process.txt](example_process.txt) — `linuxlab process --iters 40 --warmup 5`
- [example_false_sharing.txt](example_false_sharing.txt) — `linuxlab false-sharing --iters 7 --warmup 2`

## How to read them

Prefer **p50** when stddev is large. The posix_spawn run below has a mean
pulled up by a 5 ms outlier; the median is the create path, the tail is the
scheduler. Histograms are linear bins up to p99 so one outlier cannot flatten
every bar. The last bin is the above-p99 overflow.

Batched experiments (syscall, false sharing, contention) report percentiles of
**batch means**. A p99 there is not the p99 of a single operation.

## Process creation (40 samples)

| variant | p50 | p90 | mean |
|---|---:|---:|---:|
| fork + `_exit` | 166 µs | 220 µs | 183 µs |
| fork + exec `/bin/true` | 1.29 ms | 1.38 ms | 1.26 ms |
| posix_spawn `/bin/true` | 1.10 ms | 1.45 ms | 1.36 ms |

fork+exec is about 8× fork alone. posix_spawn's median beats fork+exec by
~15% in this small process; exec of `/bin/true` dominates, and the page-table
copy posix_spawn avoids is not the biggest term until the parent is large.
The mean of posix_spawn is *worse* than fork+exec because of one multi-ms
sample. Quoting the mean would reverse the result. That is why the lab prints
the distribution.

## False sharing (7 samples, 300k RMWs each, two cores)

| layout | p50 per RMW | throughput |
|---|---:|---:|
| both atomics on one line | 38.0 ns | 53 M RMW/s |
| padded to 64 B | 6.2 ns | 321 M RMW/s |
| padded to 128 B | 6.5 ns | 310 M RMW/s |

Same instruction stream. The packed case is **6.0×** slower. 64 B and 128 B
match, so on this CPU the adjacent-line prefetcher was not the coupling.
The ratio is the result; the absolute nanoseconds are not portable.

## Other shapes observed on the same guest

These are order-of-magnitude checks from `--quick` runs, so the sample count
is small. Re-run before you quote them.

- Pipe ping-pong, both threads pinned to one core: ~2.3 µs per round trip
  (~1.1 µs one way). Same pipe across two cores: ~28 µs per round trip.
  For a blocking handoff, local wakeup beats an IPI.
- `sched_yield` pair pinned to one core: ~0.6 µs per yield.
- Mutex token ring, 2 threads, one core: ~1.3 µs per handoff. Spread across
  two cores: ~15 µs.
- Uncontended `std::mutex`: ~27 ns. Uncontended TTAS spin: ~15 ns. At 4
  threads on 2 cores an *empty* critical section stays in userspace (adaptive
  spinning); it does not become a context switch. The sleep path is the
  mutex ring, not the empty increment.
- Ticket lock under contention was several times a TTAS lock. Fairness cost.
- First touch of an anonymous mapping: ~1 µs/page. Second touch: ~6 ns/page.
  Ratio ~170×. Lazy `mmap`+`munmap` of 2 MiB: ~1.3 µs for the calls.
- libc `clock_gettime(CLOCK_MONOTONIC)` was several times cheaper than
  `syscall(SYS_clock_gettime)` (vDSO). libc `getpid` matched
  `syscall(SYS_getpid)` at ~80 ns: this glibc does not cache getpid (removed
  in glibc 2.25).
- Independent compute, 2 threads: pinning both to cpu 0 made the makespan
  ~2.0× the spread-pinned run. Unpinned matched spread.
- Userspace flag ping-pong with no yield: ~0.6 µs/round-trip across two
  cores, and ~10,000× that on one core, because the waiter waits for
  preemption.
