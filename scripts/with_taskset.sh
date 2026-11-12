#!/usr/bin/env bash
# Pin the whole process, then run linuxlab.
#   CPUSET=0 scripts/with_taskset.sh contention --threads 4
#   CPUSET=0,1 scripts/with_taskset.sh false-sharing
# Per-thread placement is done inside the affinity and context experiments
# with sched_setaffinity. taskset only sets the mask the process starts with.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${BIN:-./build/linuxlab}"
CPUSET="${CPUSET:-0}"
if [[ ! -x "$BIN" ]]; then
    echo "binary not found at $BIN" >&2
    exit 1
fi
if ! command -v taskset >/dev/null 2>&1; then
    echo "taskset is not installed (package util-linux)." >&2
    exit 1
fi

echo "pinning the process to CPUs: ${CPUSET}"
exec taskset -c "$CPUSET" "$BIN" "$@"
