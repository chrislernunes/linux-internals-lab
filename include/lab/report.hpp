#pragma once

#include "lab/config.hpp"
#include "lab/stats.hpp"
#include "lab/timer.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace lab {

inline void print_title(const std::string& title) {
    std::cout << "\n============================================================\n"
              << title
              << "\n============================================================\n";
}

inline void print_why(const std::string& text) {
    std::cout << "\nwhy:\n" << text << "\n";
}

struct Unit {
    double scale;  // display = nanoseconds / scale
    const char* name;
};

inline Unit choose_unit(double ns) {
    if (ns >= 1.0e6) {
        return {1.0e6, "ms"};
    }
    if (ns >= 1.0e3) {
        return {1.0e3, "us"};
    }
    return {1.0, "ns"};
}

inline int precision_for(double display_value) {
    const double v = display_value < 0 ? -display_value : display_value;
    if (v >= 100.0) {
        return 1;
    }
    if (v >= 1.0) {
        return 2;
    }
    return 3;
}

// samples_ns holds one number per trial. If each trial timed `ops_per_sample`
// operations, every printed latency is divided by that count.
// For batched trials, percentiles describe the distribution of batch means,
// not the tail of a single operation. Callers say so in the label.
inline Summary emit(const Config& cfg,
                    const std::string& experiment,
                    const std::string& variant,
                    const std::vector<double>& samples_ns,
                    double ops_per_sample,
                    const std::string& op_name,
                    double throughput_scale = 1.0) {
    const Summary raw = summarize(samples_ns);
    if (raw.n == 0 || !(ops_per_sample > 0.0)) {
        std::cout << "\n  " << variant << "\n  (no samples)\n";
        return raw;
    }

    auto per_op = [&](double batch_ns) { return batch_ns / ops_per_sample; };
    const double p50_op = per_op(raw.p50);
    const Unit unit = choose_unit(p50_op > 0.0 ? p50_op : per_op(raw.mean));
    const int prec = precision_for(p50_op / unit.scale);

    auto show = [&](double batch_ns) { return per_op(batch_ns) / unit.scale; };

    const auto flags = std::cout.flags();
    const auto old_prec = std::cout.precision();

    std::cout << "\n  " << variant << "\n";
    std::cout << "  samples=" << raw.n << "  ops/sample=" << ops_per_sample
              << "  unit=" << unit.name << " per " << op_name << "\n";
    std::cout << std::right << std::fixed << std::setprecision(prec);
    std::cout << "  " << std::setw(12) << "min" << std::setw(12) << "p50" << std::setw(12)
              << "p90" << std::setw(12) << "p99" << std::setw(12) << "max" << std::setw(12)
              << "mean" << std::setw(12) << "stddev"
              << "\n";
    std::cout << "  " << std::setw(12) << show(raw.min) << std::setw(12) << show(raw.p50)
              << std::setw(12) << show(raw.p90) << std::setw(12) << show(raw.p99)
              << std::setw(12) << show(raw.max) << std::setw(12) << show(raw.mean)
              << std::setw(12) << (raw.stddev / ops_per_sample) / unit.scale << "\n";

    double throughput = 0.0;
    if (raw.sum > 0.0) {
        const double total_ops =
            ops_per_sample * throughput_scale * static_cast<double>(raw.n);
        const double seconds = raw.sum / 1.0e9;
        throughput = total_ops / seconds;
        std::cout << std::setprecision(throughput >= 100.0 ? 0 : 2);
        std::cout << "  throughput: " << throughput << " " << op_name << "/s\n";
    }

    if (raw.mean > 0.0 && raw.stddev > raw.mean) {
        std::cout << "  note: stddev > mean. The machine is noisy or the operation\n"
                     "        sometimes blocks. Prefer p50/p90 over the mean.\n";
    }

    std::vector<double> display;
    display.reserve(samples_ns.size());
    for (double ns : samples_ns) {
        display.push_back(per_op(ns) / unit.scale);
    }
    const auto bins = histogram(display);
    std::size_t peak = 1;
    for (const auto& bin : bins) {
        peak = std::max(peak, bin.count);
    }
    std::cout << "  histogram (" << unit.name << ", bins up to p99, last is above p99):\n";
    std::cout << std::setprecision(prec);
    for (const auto& bin : bins) {
        if (bin.overflow && bin.count == 0) {
            continue;
        }
        const int width = static_cast<int>((40.0 * static_cast<double>(bin.count)) /
                                            static_cast<double>(peak));
        std::cout << "   [" << std::setw(10) << bin.lo << ", " << std::setw(10) << bin.hi
                  << (bin.overflow ? "] " : ") ");
        std::cout << std::string(static_cast<std::size_t>(width), '#') << " " << bin.count;
        if (bin.overflow) {
            std::cout << "  above-p99";
        }
        std::cout << "\n";
    }

    std::cout.flags(flags);
    std::cout.precision(old_prec);

    if (!cfg.csv.empty()) {
        bool nonempty = false;
        {
            std::ifstream in(cfg.csv);
            nonempty = in.good() && in.peek() != std::ifstream::traits_type::eof();
        }
        std::ofstream out(cfg.csv, std::ios::app);
        if (!out) {
            std::cerr << "warning: could not append CSV " << cfg.csv << "\n";
        } else {
            if (!nonempty) {
                out << "experiment,variant,samples,ops_per_sample,"
                       "min_ns,p50_ns,p90_ns,p99_ns,max_ns,mean_ns,stddev_ns,"
                       "throughput_per_s\n";
            }
            out << experiment << "," << variant << "," << raw.n << "," << ops_per_sample << ","
                << per_op(raw.min) << "," << per_op(raw.p50) << "," << per_op(raw.p90) << ","
                << per_op(raw.p99) << "," << per_op(raw.max) << "," << per_op(raw.mean) << ","
                << (raw.stddev / ops_per_sample) << "," << throughput << "\n";
        }
    }
    return raw;
}

template <class F>
std::vector<double> collect(int warmup, int iters, F&& one_sample) {
    for (int i = 0; i < warmup; ++i) {
        one_sample();
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        samples.push_back(one_sample());
    }
    return samples;
}

}  // namespace lab
