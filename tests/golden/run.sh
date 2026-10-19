#!/usr/bin/env bash
# Compares `reliure-cli list` with `calibredb list --for-machine --fields all`,
# each on its own copy of the fixture library and, with RELIURE_GOLDEN_REAL=1,
# of the library Calibre opens; and `reliure-cli read` with Calibre's readers
# on book files. calibredb can rename files and touches
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

# Runs a Calibre oracle; a failure is reported with its output, not hidden.
oracle() {
    local log="$work/oracle.log"
    if ! calibre-debug -e "$@" > "$log" 2>&1; then
        echo "calibre-debug -e $(basename "$1") failed:"
        tail -20 "$log"
        status=1
        return 1
    fi
}

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

# What adding a file reads from it, Calibre's readers against Reliure's, on
# the book files of folders: the fixture library's and tests/data/import.
reads() {
    local name=$1 files=()
    shift
    while IFS= read -r -d '' file; do
        files+=("$file")
    done < <(find "$@" -mindepth 1 -name '.*' -prune -o -type f ! -name '*.db' ! -name 'metadata.opf' \
        ! -name 'cover.jpg' ! -iname '*conflicted copy*' ! -iname '*copie en conflit*' -print0 | sort -z)
    if [ ${#files[@]} -eq 0 ]; then
        echo "$name reads: no book files"
        status=1
        return
    fi
    oracle "$root/tools/oracle/read_oracle.py" -- "$work/$name-read-calibre.json" "${files[@]}" || return 0
    if ! "$cli" read "${files[@]}" > "$work/$name-read-reliure.json" 2> "$work/$name-read.log"; then
        echo "$name reads: reliure-cli read failed:"
        tail -20 "$work/$name-read.log"
        status=1
        return 0
    fi
    if python3 "$root/tests/golden/compare_reads.py" "$work/$name-read-calibre.json" "$work/$name-read-reliure.json" \
        > "$work/$name-reads.txt"; then
        echo "$name reads: $(tail -1 "$work/$name-reads.txt")"
    else
        echo "$name reads: Reliure reads files differently:"
        head -60 "$work/$name-reads.txt"
        status=1
    fi
}

reads fixture "$root/tests/data/import" "$root/tests/data/library"

# The same edits written by Calibre and by Reliure, then compared table by
# table and file by file; calibredb then checks Reliure's copy and lists it as
# it lists Calibre's.
edits() {
    local edits=$1
    for side in calibre reliure; do
        rm -rf "$work/edit-$side"
        cp -a --reflink=auto "$root/tests/data/library" "$work/edit-$side"
    done
    oracle "$root/tools/oracle/apply_edits.py" -- "$work/edit-calibre" "$edits" || return 0
    if ! "$cli" apply --force --library "$work/edit-reliure" "$edits" >/dev/null; then
        status=1
        return 0
    fi
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

# Books and formats added, moved to the trash and restored, step by step on
# both copies, each step compared.
operations() {
    for side in calibre reliure; do
        rm -rf "$work/ops-$side"
        cp -a --reflink=auto "$root/tests/data/library" "$work/ops-$side"
    done
    local ops
    for ops in "$@"; do
        echo "$(basename "$ops"):"
        oracle "$root/tools/oracle/apply_edits.py" -- "$work/ops-calibre" "$ops" || return 0
        if ! "$cli" apply --force --library "$work/ops-reliure" "$ops" >/dev/null; then
            status=1
            return
        fi
        if ! python3 "$root/tests/golden/compare_libraries.py" --added-from 7 "$work/ops-calibre" "$work/ops-reliure"; then
            status=1
        fi
        for side in calibre reliure; do
            calibredb check_library --with-library "$work/ops-$side" > "$work/ops-check-$side.txt" 2>&1 || true
        done
        if ! cmp -s "$work/ops-check-calibre.txt" "$work/ops-check-reliure.txt"; then
            echo "calibredb check_library reports differently:"
            diff -u "$work/ops-check-calibre.txt" "$work/ops-check-reliure.txt" | head -30
            status=1
        fi
        for side in calibre reliure; do
            cp -a --reflink=auto "$work/ops-$side" "$work/after-$(basename "$ops" .json)-$side"
        done
    done
}

operations "$root/tests/data/edits/ops-1.json" "$root/tests/data/edits/ops-2.json" "$root/tests/data/edits/ops-3.json"

# Each restores what the other moved to the trash: the second step applied by
# Calibre to Reliure's copy, and by Reliure to Calibre's, ends as the other.
cross() {
    rm -rf "$work/cross-calibre" "$work/cross-reliure"
    cp -a --reflink=auto "$work/after-ops-1-reliure" "$work/cross-calibre"
    cp -a --reflink=auto "$work/after-ops-1-calibre" "$work/cross-reliure"
    oracle "$root/tools/oracle/apply_edits.py" -- "$work/cross-calibre" "$root/tests/data/edits/ops-2.json" || return 0
    if ! "$cli" apply --force --library "$work/cross-reliure" "$root/tests/data/edits/ops-2.json" >/dev/null; then
        status=1
        return 0
    fi
    echo "Calibre restoring Reliure's trash:"
    python3 "$root/tests/golden/compare_libraries.py" --added-from 7 "$work/after-ops-2-calibre" "$work/cross-calibre" \
        || status=1
    echo "Reliure restoring Calibre's trash:"
    python3 "$root/tests/golden/compare_libraries.py" --added-from 7 "$work/after-ops-2-reliure" "$work/cross-reliure" \
        || status=1
}

cross

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
        reads real "$work/real-reliure"
    fi
fi

exit $status
