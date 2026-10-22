#!/usr/bin/env bash
# Prints how many compilers a build can run at once: one per core, but no more
# than the available memory holds at 1 GB each, about the most one takes. A
# build runs about 100 compilers, so an unbounded make -j runs out of memory.
set -euo pipefail

cores=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
if [[ -r /proc/meminfo ]]; then
    memory_mb=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo)
else
    # macOS has no figure for available memory: count half of it.
    memory_mb=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 2147483648) / 2097152 ))
fi

jobs=$(( ${memory_mb:-1024} / 1024 ))
if (( jobs > cores )); then
    jobs=$cores
fi
if (( jobs < 1 )); then
    jobs=1
fi
echo "$jobs"
