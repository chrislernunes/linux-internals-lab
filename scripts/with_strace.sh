#!/usr/bin/env bash
# Summarize syscalls, or write a full trace.
#   scripts/with_strace.sh syscall --quick
#   STRACE_MODE=trace scripts/with_strace.sh process --iters 5
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${BIN:-./build/linuxlab}"
if [[ ! -x "$BIN" ]]; then
    echo "binary not found at $BIN" >&2
    exit 1
fi
if ! command -v strace >/dev/null 2>&1; then
    echo "strace is not installed. sudo apt install strace" >&2
    exit 1
fi

mkdir -p results
if [[ "${STRACE_MODE:-summary}" == "trace" ]]; then
    out="${STRACE_OUT:-results/strace.txt}"
    echo "writing ${out}"
    exec strace -f -o "$out" "$BIN" "$@"
else
    # -c counts calls. -f follows children, which process creation needs.
    exec strace -c -f "$BIN" "$@"
fi
