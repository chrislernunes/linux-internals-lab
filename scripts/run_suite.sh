#!/usr/bin/env bash
# Run every experiment and append a CSV of nanosecond statistics.
# Extra arguments are forwarded to linuxlab (for example --quick or --iters 20).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${BIN:-./build/linuxlab}"
if [[ ! -x "$BIN" ]]; then
    echo "binary not found at $BIN" >&2
    echo "build first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
    exit 1
fi

mkdir -p results
stamp="$(date +%Y%m%d-%H%M%S)"
csv="results/suite-${stamp}.csv"
echo "writing ${csv}"
"$BIN" all --csv "$csv" "$@"
ln -sfn "$(basename "$csv")" results/latest.csv
echo "latest -> results/latest.csv"
