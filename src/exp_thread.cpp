#include "experiments.hpp"

#include "lab/report.hpp"

#include <iostream>
#include <pthread.h>
#include <thread>

namespace {

void empty_fn() {}

void* empty_pthread(void*) { return nullptr; }

double time_std_thread() {
    return lab::time_ns([&] {
        std::thread worker(empty_fn);
        worker.join();
    });
}

double time_pthread(bool small_stack, bool& ok) {
    ok = true;
    return lab::time_ns([&] {
        pthread_attr_t attr;
        pthread_attr_t* attr_ptr = nullptr;
        if (small_stack) {
            if (pthread_attr_init(&attr) != 0) {
                ok = false;
                return;
            }
            // Well above PTHREAD_STACK_MIN. The default is usually 8 MiB of
            // virtual address space, not 8 MiB of committed pages.
            if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0) {
                pthread_attr_destroy(&attr);
                ok = false;
                return;
            }
            attr_ptr = &attr;
        }
        pthread_t thread;
        const int rc = pthread_create(&thread, attr_ptr, empty_pthread, nullptr);
        if (attr_ptr != nullptr) {
            pthread_attr_destroy(&attr);
        }
        if (rc != 0) {
            ok = false;
            return;
        }
        if (pthread_join(thread, nullptr) != 0) {
            ok = false;
        }
    });
}

}  // namespace

int exp_thread(const lab::Config& cfg) {
    lab::print_title("2. thread creation and destruction");
    const int iters = lab::pick_count(cfg.iters, cfg.quick ? 20 : 200);
    const int warmup = lab::pick_count(cfg.warmup, cfg.quick ? 2 : 20);
    std::cout << "  Each sample is one create + start + exit + join.\n"
              << "  Only one extra thread is alive at a time, so this does not\n"
              << "  hit nproc or RLIMIT_NPROC.\n";

    int rc = 0;
    {
        const auto samples = lab::collect(warmup, iters, [] { return time_std_thread(); });
        lab::emit(cfg, "thread", "std::thread create+join", samples, 1.0, "thread");
    }
    {
        bool failed = false;
        const auto samples = lab::collect(warmup, iters, [&] {
            bool ok = true;
            const double ns = time_pthread(false, ok);
            if (!ok) failed = true;
            return ns;
        });
        if (failed) {
            std::cerr << "  pthread create+join failed\n";
            rc = 1;
        }
        lab::emit(cfg, "thread", "pthread_create+join (default stack)", samples, 1.0, "thread");
    }
    {
        bool failed = false;
        const auto samples = lab::collect(warmup, iters, [&] {
            bool ok = true;
            const double ns = time_pthread(true, ok);
            if (!ok) failed = true;
            return ns;
        });
        if (failed) {
            std::cerr << "  pthread small-stack create failed\n";
            rc = 1;
        }
        lab::emit(cfg, "thread", "pthread_create+join (256 KiB stack)", samples, 1.0, "thread");
    }

    lab::print_why(
        "  pthread_create is clone() with CLONE_VM | CLONE_FS | CLONE_FILES |\n"
        "  CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |\n"
        "  CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID. The new task shares the\n"
        "  address space, so there is no page-table copy and no TLB flush of\n"
        "  the kind fork pays. What remains is allocating a task_struct, a\n"
        "  kernel stack, scheduling the thread on, and later joining it.\n"
        "  Join waits on a futex that the kernel stores into the tid word at\n"
        "  exit (CHILD_CLEARTID). That wakeup is part of the number above.\n"
        "  The default stack is a virtual mapping (often 8 MiB) plus a guard\n"
        "  page. glibc mmaps it on the slow path. Pages are not committed until\n"
        "  touched, and an empty thread touches only a few, so this is not\n"
        "  8 MiB of faults — that would show up as hundreds of microseconds,\n"
        "  which is the memory experiment's first-touch regime. A gap of tens\n"
        "  of microseconds between the default stack and a 256 KiB stack is\n"
        "  the cost of mapping a larger VMA, not of RSS. Measure it; do not\n"
        "  assume the gap is zero. std::thread on libstdc++ is the same pthread\n"
        "  path plus a small allocation for the callable, so it should track\n"
        "  the default-stack pthread number within noise.\n"
        "  Detached threads are omitted: without a join there is no correct\n"
        "  'finished' edge that does not itself add a synchronization variable.");
    return rc;
}
