#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace lab {

struct Summary {
    std::size_t n = 0;
    double min = 0;
    double max = 0;
    double mean = 0;
    double stddev = 0;  // sample standard deviation (n-1)
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double sum = 0;
};

// Linear interpolation between the closest ranks. For a sorted sample of
// size n, percentile p sits at rank (p/100)*(n-1). Documented so a reader
// can reproduce the numbers in a notebook.
inline double percentile_sorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double rank = (p / 100.0) * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(rank);
    const auto hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

inline Summary summarize(std::vector<double> values) {
    Summary s;
    s.n = values.size();
    if (values.empty()) {
        return s;
    }
    s.sum = std::accumulate(values.begin(), values.end(), 0.0);
    s.mean = s.sum / static_cast<double>(values.size());
    if (values.size() > 1) {
        double acc = 0.0;
        for (double x : values) {
            const double d = x - s.mean;
            acc += d * d;
        }
        s.stddev = std::sqrt(acc / static_cast<double>(values.size() - 1));
    }
    std::sort(values.begin(), values.end());
    s.min = values.front();
    s.max = values.back();
    s.p50 = percentile_sorted(values, 50.0);
    s.p90 = percentile_sorted(values, 90.0);
    s.p99 = percentile_sorted(values, 99.0);
    return s;
}

struct HistBin {
    double lo = 0;
    double hi = 0;
    std::size_t count = 0;
    bool overflow = false;  // above p99, so one huge outlier cannot stretch every bin
};

inline std::vector<HistBin> histogram(const std::vector<double>& raw, int bins = 8) {
    std::vector<HistBin> out;
    if (raw.empty() || bins < 1) {
        return out;
    }
    std::vector<double> values = raw;
    std::sort(values.begin(), values.end());
    const double lo = values.front();
    const double p99 = percentile_sorted(values, 99.0);
    const double hi = std::max(p99, lo);
    const double width = (hi - lo) / static_cast<double>(bins);

    out.resize(static_cast<std::size_t>(bins) + 1);
    for (int i = 0; i < bins; ++i) {
        const double a = lo + width * static_cast<double>(i);
        const double b = (i + 1 == bins) ? hi : lo + width * static_cast<double>(i + 1);
        out[static_cast<std::size_t>(i)] = HistBin{a, b, 0, false};
    }
    out.back() = HistBin{hi, values.back(), 0, true};

    for (double x : values) {
        int idx = 0;
        if (x > hi) {
            idx = bins;
        } else if (!(width > 0.0)) {
            idx = 0;
        } else {
            idx = static_cast<int>((x - lo) / width);
            if (idx < 0) {
                idx = 0;
            }
            if (idx >= bins) {
                idx = bins - 1;
            }
        }
        out[static_cast<std::size_t>(idx)].count++;
    }
    return out;
}

}  // namespace lab
