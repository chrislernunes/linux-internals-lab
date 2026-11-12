#pragma once

#include "lab/config.hpp"

int self_test();
int exp_process(const lab::Config& cfg);
int exp_thread(const lab::Config& cfg);
int exp_context(const lab::Config& cfg);
int exp_contention(const lab::Config& cfg);
int exp_false_sharing(const lab::Config& cfg);
int exp_memory(const lab::Config& cfg);
int exp_affinity(const lab::Config& cfg);
int exp_syscall(const lab::Config& cfg);
