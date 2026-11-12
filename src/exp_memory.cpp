#include "experiments.hpp"

#include "lab/affinity.hpp"
#include "lab/report.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <vector>

namespace {

std::size_t map_len(const lab::Config& cfg) {
    const std::size_t page = static_cast<std::size_t>(lab::page_size());
    const std::size_t mib = static_cast<std::size_t>(lab::inner_work(cfg, 8, 2));
    const std::size_t bytes = mib << 20;
    return (bytes + page - 1) / page * page;
}

void touch_pages(void* p, std::size_t bytes) {
    const std::size_t page = static_cast<std::size_t>(lab::page_size());
    auto* c = static_cast<volatile char*>(p);
    for (std::size_t off = 0; off < bytes; off += page) {
        c[off] = 1;
    }
}

void* map_anon(std::size_t bytes, int extra_flags) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | extra_flags, -1, 0);
    if (p == MAP_FAILED) {
        throw std::runtime_error(std::string("mmap: ") + std::strerror(errno));
    }
    return p;
}

void no_thp(void* p, std::size_t bytes) {
    // Best effort. Failure just means THP may collapse the fault stream.
    ::madvise(p, bytes, MADV_NOHUGEPAGE);
}

}  // namespace

int exp_memory(const lab::Config& cfg) {
    lab::print_title("6. allocation and page faults");
    const int samples = lab::pick_count(cfg.iters, cfg.quick ? 4 : 16);
    const int warmup = lab::pick_count(cfg.warmup, cfg.quick ? 1 : 2);
    const int small_batch = lab::inner_work(cfg, 2000, 200);
    const std::size_t bytes = map_len(cfg);
    const std::size_t pages = bytes / static_cast<std::size_t>(lab::page_size());

    std::cout << "  Region under test: " << (bytes / (1024 * 1024)) << " MiB (" << pages
              << " pages). Small-alloc batch: " << small_batch << ".\n"
              << "  MADV_NOHUGEPAGE is requested so transparent huge pages do not\n"
              << "  turn the first-touch loop into a handful of 2 MiB faults.\n";

    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                for (int i = 0; i < small_batch; ++i) {
                    void* p = ::operator new(64);
                    *static_cast<volatile char*>(p) = 1;
                    ::operator delete(p);
                }
            });
        });
        lab::emit(cfg, "memory", "operator new/delete 64B + touch", data,
                  static_cast<double>(small_batch), "alloc");
    }

    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                void* p = map_anon(bytes, 0);
                ::munmap(p, bytes);
            });
        });
        lab::emit(cfg, "memory", "mmap+munmap anonymous, no touch (lazy)", data, 1.0, "mmap");
    }

    lab::Summary first_s;
    lab::Summary second_s;
    {
        const auto data = lab::collect(warmup, samples, [&] {
            void* p = map_anon(bytes, 0);
            no_thp(p, bytes);
            const double ns = lab::time_ns([&] { touch_pages(p, bytes); });
            ::munmap(p, bytes);
            return ns;
        });
        first_s = lab::emit(cfg, "memory", "first touch after lazy mmap", data,
                            static_cast<double>(pages), "page");
    }
    {
        const auto data = lab::collect(warmup, samples, [&] {
            void* p = map_anon(bytes, 0);
            no_thp(p, bytes);
            touch_pages(p, bytes);
            const double ns = lab::time_ns([&] { touch_pages(p, bytes); });
            ::munmap(p, bytes);
            return ns;
        });
        second_s = lab::emit(cfg, "memory", "second touch, pages already resident", data,
                             static_cast<double>(pages), "page");
    }
    if (second_s.mean > 0.0) {
        std::cout << "\n  ratio mean(first touch) / mean(second touch) per page = "
                  << (first_s.mean / second_s.mean) << "\n";
    }

    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                void* p = map_anon(bytes, MAP_POPULATE);
                ::munmap(p, bytes);
            });
        });
        lab::emit(cfg, "memory", "mmap MAP_POPULATE + munmap", data, static_cast<double>(pages),
                  "page");
    }

    {
        const auto data = lab::collect(warmup, samples, [&] {
            return lab::time_ns([&] {
                void* p = ::operator new(bytes);
                no_thp(p, bytes);
                touch_pages(p, bytes);
                ::operator delete(p);
            });
        });
        lab::emit(cfg, "memory", "operator new large + first touch + delete", data,
                  static_cast<double>(pages), "page");
    }

    {
        rlimit lim{};
        if (getrlimit(RLIMIT_MEMLOCK, &lim) == 0) {
            std::cout << "\n  RLIMIT_MEMLOCK soft=" << lim.rlim_cur << " hard=" << lim.rlim_max
                      << " bytes\n";
        }
        void* p = map_anon(bytes, 0);
        no_thp(p, bytes);
        const int rc = ::mlock(p, bytes);
        if (rc != 0) {
            std::cout << "  skip: mlock (" << std::strerror(errno)
                      << "). Unprivileged processes usually cannot pin this much.\n";
            ::munmap(p, bytes);
        } else {
            ::munlock(p, bytes);
            ::munmap(p, bytes);
            const auto data = lab::collect(warmup, samples, [&] {
                void* q = map_anon(bytes, 0);
                no_thp(q, bytes);
                const double ns = lab::time_ns([&] {
                    if (::mlock(q, bytes) != 0) {
                        throw std::runtime_error("mlock failed after a successful probe");
                    }
                });
                ::munlock(q, bytes);
                ::munmap(q, bytes);
                return ns;
            });
            lab::emit(cfg, "memory", "mlock of fresh anonymous mapping", data,
                      static_cast<double>(pages), "page");
        }
    }

    lab::print_why(
        "  Anonymous mmap installs a VMA and almost nothing else. The physical\n"
        "  page is allocated on first touch, in the fault handler: allocate a\n"
        "  frame (or a shared zero page for a read), fill the PTE, invalidate\n"
        "  the TLB entry. That is why the mmap call itself is cheap and the\n"
        "  first write loop is not. The second write hits resident pages and\n"
        "  is store bandwidth plus TLB hits.\n"
        "  Do not divide the first-touch time by 4096 and call it 'the' page-fault\n"
        "  cost. Linux fault-around (do_anonymous_page's neighbour prefault, in\n"
        "  file faults via do_fault_around) maps a cluster of nearby pages on one\n"
        "  fault, and the allocator may hand out an already-zeroed page. The\n"
        "  ratio against the second touch is the number that matters.\n"
        "  MAP_POPULATE faults inside mmap, before it returns. mlock does the\n"
        "  same and then pins the frames so they cannot be reclaimed. The pin\n"
        "  is capped by RLIMIT_MEMLOCK; without CAP_IPC_LOCK a large mlock\n"
        "  returns EPERM or ENOMEM. This lab treats that as a skip, not a failure.\n"
        "  Small operator new is the thread arena (brk/mmap once, then a freelist).\n"
        "  After warmup it does not fault per call. Allocations above glibc's\n"
        "  M_MMAP_THRESHOLD (default 128 KiB, see mallopt) are their own mmap and\n"
        "  are unmapped on free, which is why the large new+touch path looks like\n"
        "  the first-touch path plus a syscall pair.\n"
        "  WSL2 adds a host transition to the fault path, so microseconds per page\n"
        "  run higher than on bare metal. The shape of the result (lazy mmap <<\n"
        "  first touch >> second touch) is the part worth defending in an interview.");
    return 0;
}
