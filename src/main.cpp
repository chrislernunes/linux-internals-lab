#include "experiments.hpp"

#include "lab/affinity.hpp"
#include "lab/timer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage() {
    std::cout
        << "usage: linuxlab <command> [options]\n"
        << "\n"
        << "commands:\n"
        << "  all             run every experiment\n"
        << "  process         fork / exec / posix_spawn cost\n"
        << "  thread          std::thread and pthread create+join\n"
        << "  context         pipe, yield, and mutex context switches\n"
        << "  contention      mutex vs spin vs ticket vs atomic\n"
        << "  false-sharing   same cache line vs padded atomics\n"
        << "  memory          malloc, mmap, first-touch faults, mlock\n"
        << "  affinity        pinning, coherence ping-pong, compute throughput\n"
        << "  syscall         libc vs raw syscall vs vDSO\n"
        << "  self-test       check the statistics helpers\n"
        << "  list            list commands\n"
        << "\n"
        << "options:\n"
        << "  --iters N       samples per variant (default: per experiment)\n"
        << "  --warmup N      discarded warmup iterations\n"
        << "  --threads N     force thread count where a sweep is used\n"
        << "  --csv PATH      append machine-readable rows (nanoseconds)\n"
        << "  --quick         shrink inner loops for a smoke run\n";
}

lab::Config parse(int argc, char** argv, std::string& command) {
    if (argc < 2) {
        command = "help";
        return {};
    }
    command = argv[1];
    lab::Config cfg;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + flag);
            }
            return std::string(argv[++i]);
        };
        if (arg == "--iters") {
            cfg.iters = std::stoi(need("--iters"));
        } else if (arg == "--warmup") {
            cfg.warmup = std::stoi(need("--warmup"));
        } else if (arg == "--threads") {
            cfg.threads = std::stoi(need("--threads"));
        } else if (arg == "--csv") {
            cfg.csv = need("--csv");
        } else if (arg == "--quick") {
            cfg.quick = true;
        } else if (arg == "-h" || arg == "--help") {
            command = "help";
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (cfg.iters < 0 || cfg.warmup < 0 || cfg.threads < 0) {
        throw std::runtime_error("counts must be >= 0");
    }
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string command;
        const lab::Config cfg = parse(argc, argv, command);
        if (command == "help" || command == "-h" || command == "--help") {
            usage();
            return 0;
        }
        if (command == "self-test") {
            return self_test();
        }
        if (command == "list") {
            std::cout << "process thread context contention false-sharing memory affinity syscall\n";
            return 0;
        }

        lab::tsc().calibrate();
        lab::print_environment();
        if (cfg.quick) {
            std::cout << "mode: quick (inner loops reduced; fine for a smoke test, not a paper)\n";
        }

        if (command == "all") {
            int rc = 0;
            rc |= exp_process(cfg);
            rc |= exp_thread(cfg);
            rc |= exp_context(cfg);
            rc |= exp_contention(cfg);
            rc |= exp_false_sharing(cfg);
            rc |= exp_memory(cfg);
            rc |= exp_affinity(cfg);
            rc |= exp_syscall(cfg);
            std::cout << "\n" << (rc == 0 ? "suite finished\n" : "suite finished with errors\n");
            return rc == 0 ? 0 : 1;
        }
        if (command == "process") return exp_process(cfg);
        if (command == "thread") return exp_thread(cfg);
        if (command == "context") return exp_context(cfg);
        if (command == "contention") return exp_contention(cfg);
        if (command == "false-sharing") return exp_false_sharing(cfg);
        if (command == "memory") return exp_memory(cfg);
        if (command == "affinity") return exp_affinity(cfg);
        if (command == "syscall") return exp_syscall(cfg);

        usage();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
