#include "experiments.hpp"

#include "lab/report.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

bool reap(pid_t pid) {
    int status = 0;
    pid_t got = -1;
    do {
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Cost of creating a process and reaping it. The child must _exit: returning
// would run C++ destructors and flush duplicated stdio buffers in the child.
double time_fork_exit(bool& ok) {
    ok = true;
    return lab::time_ns([&] {
        const pid_t pid = fork();
        if (pid < 0) {
            ok = false;
            return;
        }
        if (pid == 0) {
            _exit(0);
        }
        if (!reap(pid)) {
            ok = false;
        }
    });
}

double time_fork_exec(bool& ok) {
    ok = true;
    return lab::time_ns([&] {
        const pid_t pid = fork();
        if (pid < 0) {
            ok = false;
            return;
        }
        if (pid == 0) {
            // execl only returns on failure. _exit avoids atexit handlers.
            execl("/bin/true", "true", static_cast<char*>(nullptr));
            _exit(127);
        }
        if (!reap(pid)) {
            ok = false;
        }
    });
}

double time_posix_spawn(bool& ok) {
    ok = true;
    return lab::time_ns([&] {
        pid_t pid = -1;
        char arg0[] = "true";
        char* argv[] = {arg0, nullptr};
        const int rc = posix_spawn(&pid, "/bin/true", nullptr, nullptr, argv, environ);
        if (rc != 0) {
            ok = false;
            return;
        }
        if (!reap(pid)) {
            ok = false;
        }
    });
}

template <class F>
int run_variant(const lab::Config& cfg,
                const char* variant,
                int iters,
                int warmup,
                F&& fn) {
    bool failed = false;
    const auto samples = lab::collect(warmup, iters, [&] {
        bool ok = true;
        const double ns = fn(ok);
        if (!ok) {
            failed = true;
        }
        return ns;
    });
    if (failed) {
        std::cerr << "  " << variant << ": one or more children failed\n";
        return 1;
    }
    lab::emit(cfg, "process", variant, samples, 1.0, "creation");
    return 0;
}

}  // namespace

int exp_process(const lab::Config& cfg) {
    lab::print_title("1. process creation");
    const int iters = lab::pick_count(cfg.iters, cfg.quick ? 10 : 60);
    const int warmup = lab::pick_count(cfg.warmup, cfg.quick ? 1 : 5);
    std::cout << "  Each sample is one create + waitpid. No batching.\n"
              << "  Child paths use _exit so they do not run destructors.\n";

    int rc = 0;
    rc |= run_variant(cfg, "fork + _exit (no exec)", iters, warmup, time_fork_exit);
    rc |= run_variant(cfg, "fork + exec /bin/true + wait", iters, warmup, time_fork_exec);
    rc |= run_variant(cfg, "posix_spawn /bin/true + wait", iters, warmup, time_posix_spawn);

    lab::print_why(
        "  fork() is clone() without CLONE_VM. The kernel duplicates the\n"
        "  task_struct, the fd table, and the page tables. Pages themselves\n"
        "  are copy-on-write, so the byte copy is cheap; the page-table copy\n"
        "  is not, and it grows with the parent's address space.\n"
        "  execve() throws that address space away: it loads the ELF, maps the\n"
        "  dynamic linker, and flushes the TLB. That is why fork+exec is\n"
        "  several times fork+_exit.\n"
        "  posix_spawn() is what most runtimes should call instead of fork+exec.\n"
        "  Modern glibc implements the common case with clone(CLONE_VM|CLONE_VFORK):\n"
        "  the child borrows the parent's address space until exec, so the page\n"
        "  tables are not copied, and the parent stays stopped so that sharing\n"
        "  is safe. The saving is proportional to the parent's page tables. In\n"
        "  this small process exec dominates, and posix_spawn may only beat\n"
        "  fork+exec by a little. Allocate a multi-gigabyte buffer in the parent\n"
        "  and run again if you want the gap to open up. strace -f -e clone,clone3,execve\n"
        "  shows the flags. Raw clone() from C++ is intentionally not called:\n"
        "  the child would share or abandon glibc TLS, which corrupts the parent.\n"
        "  Reaping is a wait queue wakeup inside waitpid. Throughput is the\n"
        "  inverse of the full create-and-reap latency, not of the syscall alone.\n"
        "  A large parent (big heap, many threads' stacks) makes fork slower.\n"
        "  This process is small; do not quote these numbers as 'the' fork cost\n"
        "  of a fat server.");
    return rc;
}
