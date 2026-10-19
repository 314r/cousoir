#!/usr/bin/env bash
# Compares `reliure-cli list` with `calibredb list --for-machine --fields all`,
# each on its own copy of the fixture library and, with RELIURE_GOLDEN_REAL=1,
# of the library Calibre opens. calibredb can rename files and touches
# metadata.db even to list it, so neither ever runs on the library itself.
#
#   tests/golden/run.sh build/reliure-cli
#
# Exits 77, which ctest counts as skipped, without calibredb.
set -euo pipefail

cli=$1
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! command -v calibredb >/dev/null; then
    echo "calibredb is not installed; skipping"
    exit 77
fi

# Under the home folder by default: on a copy-on-write filesystem the copies
# of a large library then cost nothing.
base=${RELIURE_GOLDEN_TMP:-${XDG_CACHE_HOME:-$HOME/.cache}/reliure-golden}
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"' EXIT
# calibredb keeps its own preferences away from the user's.
user_config=${CALIBRE_CONFIG_DIRECTORY:-${XDG_CONFIG_HOME:-$HOME/.config}/calibre}
export CALIBRE_CONFIG_DIRECTORY="$work/config"
export CALIBRE_OVERRIDE_LANG=fr

status=0
compare() {
    local name=$1 source=$2
    for side in calibre reliure; do
        cp -a --reflink=auto "$source" "$work/$name-$side"
        # Dropbox conflict copies are not part of the library.
        find "$work/$name-$side" -maxdepth 1 -type f \
            \( -iname '*conflicted copy*' -o -iname '*copie en conflit*' -o -iname '*case conflict*' \) -delete
    done
    calibredb list --with-library "$work/$name-calibre" --for-machine --fields all \
        | sed "s|$work/$name-calibre|@LIBRARY@|g" > "$work/$name-calibre.json"
    "$cli" list --library "$work/$name-reliure" \
        | sed "s|$work/$name-reliure|@LIBRARY@|g" > "$work/$name-reliure.json"
    if cmp -s "$work/$name-calibre.json" "$work/$name-reliure.json"; then
        echo "$name: identical ($(grep -c '"id":' "$work/$name-calibre.json") books)"
    else
        echo "$name: reliure-cli list differs from calibredb list"
        diff -u "$work/$name-calibre.json" "$work/$name-reliure.json" | head -60
        status=1
    fi
}

compare fixture "$root/tests/data/library"

if [ "${RELIURE_GOLDEN_REAL:-0}" = 1 ]; then
    real=$(python3 -c "
import json, sys
print(json.load(open(sys.argv[1])).get('library_path') or '')" "$user_config/global.py.json")
    if [ -z "$real" ]; then
        echo "Calibre has no library on this computer"
        status=1
    elif grep -q "@calibre-singleinstance-$(id -u)-db" /proc/net/unix 2>/dev/null; then
        echo "Calibre is open; close it so the copy is consistent"
        status=1
    else
        compare real "$real"
    fi
fi

exit $status
