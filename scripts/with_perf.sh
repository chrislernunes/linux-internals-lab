#!/usr/bin/env bash
# Wrap any linuxlab command in `perf stat`.
#   scripts/with_perf.sh false-sharing
#   scripts/with_perf.sh syscall --quick
# Events that this kernel does not support are dropped by perf; if the whole
# command fails, check /proc/sys/kernel/perf_event_paranoid (2 or 3 blocks
# unprivileged perf — `sudo sysctl kernel.perf_event_paranoid=1`).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${BIN:-./build/linuxlab}"
if [[ ! -x "$BIN" ]]; then
    echo "binary not found at $BIN" >&2
    exit 1
fi
if ! command -v perf >/dev/null 2>&1; then
    echo "perf is not installed." >&2
    echo "Ubuntu: sudo apt install linux-tools-generic linux-tools-$(uname -r)" >&2
    echo "WSL2: the Microsoft kernel often has no matching linux-tools package." >&2
    echo "The lab still runs without perf. See docs/perf.md." >&2
    exit 1
fi

exec perf stat -d \
    -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,\
cache-references,cache-misses,LLC-loads,LLC-load-misses \
    -- "$BIN" "$@"
