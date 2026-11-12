#pragma once

#include <string>

namespace lab {

// Command-line knobs shared by every experiment.
// iters/warmup of 0 means "use the experiment's own default".
struct Config {
    int iters = 0;
    int warmup = 0;
    int threads = 0;  // 0 = experiment picks a sweep (1, ncpu, 2*ncpu)
    bool quick = false;
    std::string csv;
};

inline int pick_count(int override_value, int fallback) {
    return override_value > 0 ? override_value : fallback;
}

// Inner repetition (ops timed inside one sample). --quick shrinks this so
// CI and smoke runs finish in seconds; it does not change the statistics code.
inline int inner_work(const Config& cfg, int normal, int quick) {
    return cfg.quick ? quick : normal;
}

}  // namespace lab
