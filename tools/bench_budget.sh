#!/bin/sh
# Runs cousoir-benchmark on a fresh copy of a 50,000-book library and fails
# past the budgets of tests/bench/budgets.json. Opt-in, as it takes a minute
# and measures this computer:
#
#   COUSOIR_BENCH=1 tools/sandbox.sh ctest --test-dir build -L bench
set -eu
build=$1
root=$(cd "$(dirname "$0")/.." && pwd)
if [ "${COUSOIR_BENCH:-0}" != 1 ]; then
    echo "Measures only with COUSOIR_BENCH=1; skipped"
    exit 77
fi
libs="$build/sandbox/libs"
# The same arguments always make the same library.
[ -f "$libs/50k/metadata.db" ] || "$build/cousoir-test-tool" make-library "$libs/50k" 50000 > /dev/null
rm -rf "$libs/budget-50k"
cp -a --reflink=auto "$libs/50k" "$libs/budget-50k"
# What the copy left pending, written before a save flushes the folder.
sync -f "$libs"
QT_QPA_PLATFORM=offscreen "$build/cousoir-benchmark" --ui --budget "$root/tests/bench/budgets.json" "$libs/budget-50k"
