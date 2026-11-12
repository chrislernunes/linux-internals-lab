#include "lab/stats.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

int self_test() {
    int failed = 0;
    const std::vector<double> values{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const lab::Summary s = lab::summarize(values);
    // rank for p50 = 0.5 * 9 = 4.5 → halfway between 5 and 6.
    if (!near(s.min, 1) || !near(s.max, 10) || !near(s.mean, 5.5) || !near(s.p50, 5.5)) {
        std::cerr << "self-test: summary mismatch min=" << s.min << " p50=" << s.p50
                  << " mean=" << s.mean << "\n";
        ++failed;
    }
    if (!(s.stddev > 2.8 && s.stddev < 3.1)) {
        std::cerr << "self-test: stddev " << s.stddev << " not near sample stdev ~3.027\n";
        ++failed;
    }
    const lab::Summary one = lab::summarize({42});
    if (!near(one.p50, 42) || !near(one.p99, 42) || one.stddev != 0.0) {
        std::cerr << "self-test: single-sample summary failed\n";
        ++failed;
    }
    const lab::Summary empty = lab::summarize({});
    if (empty.n != 0) {
        std::cerr << "self-test: empty summary failed\n";
        ++failed;
    }
    const auto bins = lab::histogram(values, 4);
    std::size_t total = 0;
    for (const auto& bin : bins) {
        total += bin.count;
    }
    if (total != values.size()) {
        std::cerr << "self-test: histogram dropped samples (" << total << ")\n";
        ++failed;
    }

    if (failed == 0) {
        std::cout << "self-test: ok\n";
        return 0;
    }
    std::cerr << "self-test: " << failed << " failure(s)\n";
    return 1;
}
