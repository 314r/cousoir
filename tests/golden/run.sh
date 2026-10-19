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
# calibredb keeps its own preferences away from the user's, and reliure-cli
# its cache and backups.
user_config=${CALIBRE_CONFIG_DIRECTORY:-${XDG_CONFIG_HOME:-$HOME/.config}/calibre}
export CALIBRE_CONFIG_DIRECTORY="$work/config"
export XDG_CACHE_HOME="$work/cache" XDG_DATA_HOME="$work/data"
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

# The same edits written by Calibre and by Reliure, then compared table by
# table and file by file; calibredb then checks Reliure's copy and lists it as
# it lists Calibre's.
edits() {
    local edits=$1
    for side in calibre reliure; do
        rm -rf "$work/edit-$side"
        cp -a --reflink=auto "$root/tests/data/library" "$work/edit-$side"
    done
    calibre-debug -e "$root/tools/oracle/apply_edits.py" -- "$work/edit-calibre" "$edits" >/dev/null
    "$cli" apply --force --library "$work/edit-reliure" "$edits" >/dev/null
    if ! python3 "$root/tests/golden/compare_libraries.py" "$work/edit-calibre" "$work/edit-reliure"; then
        status=1
    fi
    for side in calibre reliure; do
        calibredb check_library --with-library "$work/edit-$side" > "$work/check-$side.txt" 2>&1 || true
        calibredb list --with-library "$work/edit-$side" --for-machine --fields all \
            | sed "s|$work/edit-$side|@LIBRARY@|g" > "$work/list-$side.json"
    done
    if ! cmp -s "$work/check-calibre.txt" "$work/check-reliure.txt"; then
        echo "calibredb check_library reports differently:"
        diff -u "$work/check-calibre.txt" "$work/check-reliure.txt" | head -30
        status=1
    fi
    if ! cmp -s "$work/list-calibre.json" "$work/list-reliure.json"; then
        echo "calibredb lists the copies differently:"
        diff -u "$work/list-calibre.json" "$work/list-reliure.json" | head -40
        status=1
    else
        echo "edits: calibredb lists both copies the same"
    fi
}

edits "$root/tests/data/edits/edits.json"

# The round trip: Calibre writes again the metadata.opf files of the books
# Reliure changed, as its GUI does when it starts, and finds nothing to
# change in them; then edits the library itself, which Reliure reads as
# calibredb does.
round_trip() {
    rm -rf "$work/trip" "$work/trip-before"
    cp -a --reflink=auto "$work/edit-reliure" "$work/trip"
    cp -a --reflink=auto "$work/trip" "$work/trip-before"
    calibredb backup_metadata --with-library "$work/trip" >/dev/null
    if ! python3 "$root/tests/golden/compare_libraries.py" --files-only "$work/trip" "$work/trip-before" >/dev/null; then
        echo "round trip: Calibre writes the metadata.opf files differently:"
        python3 "$root/tests/golden/compare_libraries.py" --files-only "$work/trip" "$work/trip-before" | head -20
        status=1
    fi
    calibredb set_metadata --with-library "$work/trip" 1 --field "title:Mood Indigo" --field "tags:Jazz,Roman" >/dev/null
    calibredb list --with-library "$work/trip" --for-machine --fields all | sed "s|$work/trip|@LIBRARY@|g" > "$work/trip-calibre.json"
    "$cli" list --library "$work/trip" | sed "s|$work/trip|@LIBRARY@|g" > "$work/trip-reliure.json"
    if cmp -s "$work/trip-calibre.json" "$work/trip-reliure.json"; then
        echo "round trip: Calibre keeps Reliure's files and Reliure reads Calibre's change"
    else
        echo "round trip: Reliure reads Calibre's change differently:"
        diff -u "$work/trip-calibre.json" "$work/trip-reliure.json" | head -30
        status=1
    fi
}

round_trip

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
