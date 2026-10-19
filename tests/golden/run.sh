#!/usr/bin/env bash
# Compares `reliure-cli list` with `calibredb list --for-machine --fields all`,
# each on its own copy of the fixture library and, with RELIURE_GOLDEN_REAL=1,
# of the library Calibre opens; `reliure-cli read` with Calibre's readers on
# book files; and the libraries Calibre and `reliure-cli apply` leave after
# the same edits, additions, removals and conversions. calibredb can rename
# files and touches metadata.db even to list it, so neither ever runs on the
# library itself.
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
    # last_modified is the time of each side's write, a second apart at times.
    for side in calibre reliure; do
        calibredb check_library --with-library "$work/edit-$side" > "$work/check-$side.txt" 2>&1 || true
        calibredb list --with-library "$work/edit-$side" --for-machine --fields all \
            | sed -e "s|$work/edit-$side|@LIBRARY@|g" -e 's|"last_modified": "[^"]*"|"last_modified": "<now>"|' \
            > "$work/list-$side.json"
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

# Books converted by Calibre's convert action and by reliure-cli, compared:
# the files of conversions by their content, without the time each stamps
# or the ids it draws. `prepare`, if given, runs on each copy first.
conversions() {
    local name=$1 source=$2 ops=$3 converted=$4 prepare=${5:-}
    for side in calibre reliure; do
        rm -rf "$work/$name-$side"
        cp -a --reflink=auto "$source" "$work/$name-$side"
        if [ -n "$prepare" ]; then
            "$prepare" "$work/$name-$side"
        fi
    done
    oracle "$root/tools/oracle/convert_books.py" -- "$work/$name-calibre" "$ops" || return 0
    if ! "$cli" apply --force --library "$work/$name-reliure" "$ops" > "$work/$name.log" 2>&1; then
        echo "$name: reliure-cli apply failed:"
        tail -20 "$work/$name.log"
        status=1
        return 0
    fi
    if python3 "$root/tests/golden/compare_libraries.py" --converted "$converted" "$work/$name-calibre" \
        "$work/$name-reliure" > "$work/$name.txt"; then
        echo "$name: $(tail -1 "$work/$name.txt")"
    else
        echo "$name: Reliure converts differently:"
        head -40 "$work/$name.txt"
        status=1
    fi
}

# Calibre draws the cover of a book without one in a random style and
# colours: one of each is left. Conversions start from Calibre's defaults
# with another output profile, as a user sets it.
mkdir -p "$work/config/conversion"
printf '%s' '{"disabled_color_themes": ["Grass", "Water", "Silver"], "disabled_styles": ["The Cross", "Half and Half", "Ornamental", "Blocks"]}' \
    > "$work/config/cover_generation.json"
printf 'json:{\n  "output_profile": "tablet"\n}' > "$work/config/conversion/page_setup.py"
conversions conversions "$root/tests/data/library" "$root/tests/data/edits/convert.json" 1:KEPUB,2:AZW3,3:AZW3,5:KEPUB \
    "$root/tests/golden/conversion_settings.py"

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

# Books sent to a reader and removed from it by Calibre's GUI and by
# reliure-cli, each on a fake reader of its own, a Kobo or a Kindle reached
# over MTP, step by step: some sent, more sent once the reader had them a
# while, one of them again, one removed. Between steps, a Kobo takes the
# books in its database (tests/golden/kobo_import.py) and Amazon blanks a
# Kindle's covers (tests/golden/kindle_sync.py), which both programs put
# back. Removing, Reliure leaves the other books' entries of
# metadata.calibre as they are, which Calibre writes again whenever it
# connects: only which books remain is compared then, and both readers once
# Calibre has connected to them. Then each program reads the other's reader:
# Calibre must take the books Reliure sent for the library's, by their uuid,
# and Reliure Calibre's.
#
#   devices <kobo|kindle> <name> <library> <books first> <books then> <book removed>
devices() {
    local kind=$1 name=$2 library=$3 side step lpath product=0 calibre_device reliure_device
    for side in calibre reliure; do
        rm -rf "$work/$name-library-$side" "$work/$name-$side"
        cp -a --reflink=auto "$library" "$work/$name-library-$side"
        if [ "$kind" = kobo ]; then
            mkdir -p "$work/$name-$side/.kobo"
            printf 'N418000000000,4.1.15,4.38.21908,4.1.15,4.1.15,00000000-0000-0000-0000-000000000390' \
                > "$work/$name-$side/.kobo/version"
            python3 -c 'import sqlite3, sys; c = sqlite3.connect(sys.argv[1]); c.executescript(open(sys.argv[2]).read())' \
                "$work/$name-$side/.kobo/KoboReader.sqlite" "$root/tests/data/kobo.sql"
        else
            mkdir -p "$work/$name-$side/documents" "$work/$name-$side/system/thumbnails"
        fi
    done
    calibre_device="$work/$name-calibre"
    reliure_device="$work/$name-reliure"
    if [ "$kind" = kindle ]; then
        product=0x9981
        calibre_device="mtpdir:$calibre_device"
        reliure_device="mtpdir:$reliure_device"
    fi
    for step in 1 2 3; do
        if [ "$step" -lt 3 ]; then
            local books=$4
            [ "$step" = 2 ] && books=$5
            printf '[{"send": %s}]' "$books" > "$work/$name-ops-calibre.json"
            printf '[{"send": {"books": %s, "device": "%s"}}]' "$books" "$reliure_device" \
                > "$work/$name-ops-reliure.json"
        else
            # The book's file as Calibre named it on its reader.
            lpath=$(python3 -c 'import json, sys
print(json.dumps([b["lpath"] for b in json.load(open(sys.argv[1])) if b.get("application_id") == int(sys.argv[2])][:1]))' \
                "$work/$name-calibre/metadata.calibre" "$6")
            printf '[{"remove": %s}]' "$lpath" > "$work/$name-ops-calibre.json"
            printf '[{"removeFromDevice": {"lpaths": %s, "device": "%s"}}]' "$lpath" "$reliure_device" \
                > "$work/$name-ops-reliure.json"
        fi
        oracle "$root/tools/oracle/send_books.py" -- "$work/$name-library-calibre" "$calibre_device" "$product" \
            "$work/$name-ops-calibre.json" || return 0
        if ! "$cli" apply --library "$work/$name-library-reliure" "$work/$name-ops-reliure.json" > "$work/$name.log" 2>&1; then
            echo "$name step $step: reliure-cli apply failed:"
            tail -20 "$work/$name.log"
            status=1
            return 0
        fi
        local only=()
        [ "$step" = 3 ] && only=(--lpaths-only)
        if python3 "$root/tests/golden/compare_devices.py" "${only[@]}" "$work/$name-calibre" "$work/$name-reliure" \
            > "$work/$name.txt"; then
            echo "$name step $step: $(tail -1 "$work/$name.txt")"
        else
            echo "$name step $step: Reliure sends differently:"
            head -40 "$work/$name.txt"
            status=1
        fi
        for side in calibre reliure; do
            if [ "$kind" = kobo ]; then
                python3 "$root/tests/golden/kobo_import.py" "$work/$name-$side"
            else
                python3 "$root/tests/golden/kindle_sync.py" "$work/$name-$side"
            fi
        done
    done
    # Each reader read by both programs, on copies.
    for side in calibre reliure; do
        rm -rf "$work/$name-read-$side"
        cp -a --reflink=auto "$work/$name-$side" "$work/$name-read-$side"
        printf '[{"matches": "%s"}]' "$work/$name-matches-$side.json" > "$work/$name-ops-matches.json"
        local read="$work/$name-read-$side" listed="$work/$name-$side"
        if [ "$kind" = kindle ]; then
            read="mtpdir:$read"
            listed="mtpdir:$listed"
        fi
        oracle "$root/tools/oracle/send_books.py" -- "$work/$name-library-calibre" "$read" "$product" \
            "$work/$name-ops-matches.json" || return 0
        "$cli" device --library "$work/$name-library-reliure" "$listed" \
            | python3 -c 'import json, sys
books = json.load(sys.stdin)["books"]
for book in books: book.pop("size")
print(json.dumps(sorted(books, key=lambda b: b["lpath"].lower()), indent=1))' > "$work/$name-listed-$side.json"
    done
    if python3 "$root/tests/golden/compare_devices.py" "$work/$name-read-calibre" "$work/$name-read-reliure" \
        > "$work/$name.txt"; then
        echo "$name once Calibre connects: $(tail -1 "$work/$name.txt")"
    else
        echo "$name once Calibre connects: the readers differ:"
        head -40 "$work/$name.txt"
        status=1
    fi
    if ! python3 -c 'import json, sys
for side in sys.argv[1:]:
    matches = json.load(open(side))
    assert matches and all(m[1] == "UUID" for m in matches), matches' \
        "$work/$name-matches-calibre.json" "$work/$name-matches-reliure.json" \
        || ! cmp -s "$work/$name-matches-calibre.json" "$work/$name-matches-reliure.json"; then
        echo "$name: Calibre does not take the books on both readers for the library's alike:"
        diff -u "$work/$name-matches-calibre.json" "$work/$name-matches-reliure.json" | head -20
        status=1
    elif ! cmp -s "$work/$name-listed-calibre.json" "$work/$name-listed-reliure.json" \
        || grep -q '"book": 0' "$work/$name-listed-calibre.json"; then
        echo "$name: Reliure does not list the books on both readers alike:"
        diff -u "$work/$name-listed-calibre.json" "$work/$name-listed-reliure.json" | head -20
        status=1
    else
        echo "$name: Calibre and Reliure each know the books the other sent ($(grep -c '"lpath"' "$work/$name-listed-calibre.json") books)"
    fi
}

devices kobo kobo "$root/tests/data/library" '[1, 2, 3, 5]' '[2, 4]' 3
devices kindle kindle "$root/tests/data/library" '[2, 5]' '[2]' 5

# A few books of the real library converted, with the user's Calibre
# settings: the two smallest EPUBs to KEPUB and AZW3, the smallest book
# with a KEPUB to KEPUB again, and the smallest whose only source is an AZW3
# to AZW3, which keeps its file as ORIGINAL_AZW3; one Calibre converted
# before, so not locked by DRM.
real_conversions() {
    local converted
    converted=$(python3 - "$work/real-reliure/metadata.db" "$work/convert-real.json" <<'PICK'
import json, sqlite3, sys
con = sqlite3.connect(f'file:{sys.argv[1]}?mode=ro', uri=True)
formats = {}
for book, fmt, size in con.execute('SELECT book, format, uncompressed_size FROM data'):
    formats.setdefault(book, {})[fmt] = size
converted = {book for (book,) in con.execute("SELECT book FROM conversion_options WHERE format='PIPE'")}
def smallest(test, key):
    found = sorted((f[key], book) for book, f in formats.items() if test(f))
    return [book for _, book in found]
epubs = smallest(lambda f: 'EPUB' in f, 'EPUB')
kepubs = smallest(lambda f: 'EPUB' in f and 'KEPUB' in f, 'EPUB')
azw3s = [book for book in smallest(lambda f: 'AZW3' in f and not ({'EPUB', 'MOBI', 'LIT', 'PRC', 'FB2', 'DOCX'} & set(f)),
                                   'AZW3') if book in converted]
picks = []
for books, fmt in ((epubs[:1], 'KEPUB'), (epubs[1:2], 'AZW3'), (kepubs[:1], 'KEPUB'), (azw3s[:1], 'AZW3')):
    picks += [(book, fmt) for book in books if (book, fmt) not in picks]
json.dump([{'convert': {'book': book, 'format': fmt}} for book, fmt in picks], open(sys.argv[2], 'w'))
print(','.join(f'{book}:{fmt}' for book, fmt in picks))
PICK
)
    # The user's conversion settings, without the rest of Calibre's.
    rm -rf "$work/config-real"
    mkdir -p "$work/config-real"
    cp "$work/config/cover_generation.json" "$work/config-real/"
    for file in global.py.json tweaks.json conversion; do
        [ -e "$user_config/$file" ] && cp -a "$user_config/$file" "$work/config-real/"
    done
    CALIBRE_CONFIG_DIRECTORY="$work/config-real" conversions real-conversions "$work/real-reliure" \
        "$work/convert-real.json" "$converted"
}

# A few books of the real library sent to a Kobo and to a Kindle with the
# user's Calibre settings: for the Kobo, the smallest EPUB without a KEPUB,
# made a KEPUB on the way, the smallest with a KEPUB and the smallest PDF;
# for the Kindle, the smallest AZW3, the smallest MOBI without one and the
# smallest PDF; the first sent again, then the second removed.
real_devices() {
    local picks kind
    for kind in kobo kindle; do
    picks=$(python3 - "$work/real-reliure/metadata.db" "$kind" <<'PICK'
import sqlite3, sys
con = sqlite3.connect(f'file:{sys.argv[1]}?mode=ro', uri=True)
formats = {}
for book, fmt, size in con.execute('SELECT book, format, uncompressed_size FROM data'):
    formats.setdefault(book, {})[fmt] = size
def smallest(test, key):
    found = sorted((f[key], book) for book, f in formats.items() if test(f))
    return found[0][1] if found else None
if sys.argv[2] == 'kobo':
    picks = [smallest(lambda f: 'EPUB' in f and 'KEPUB' not in f, 'EPUB'), smallest(lambda f: 'KEPUB' in f, 'KEPUB'),
             smallest(lambda f: 'PDF' in f and not {'KEPUB', 'EPUB'} & set(f), 'PDF')]
else:
    picks = [smallest(lambda f: 'AZW3' in f, 'AZW3'), smallest(lambda f: 'MOBI' in f and 'AZW3' not in f, 'MOBI'),
             smallest(lambda f: 'PDF' in f and not {'AZW3', 'MOBI', 'AZW'} & set(f), 'PDF')]
picks = list(dict.fromkeys(book for book in picks if book is not None))
print(f'{picks} [{picks[0]}] {picks[1] if len(picks) > 1 else picks[0]}')
PICK
)
    # The user's settings, without the rest of Calibre's.
    rm -rf "$work/config-devices"
    mkdir -p "$work/config-devices"
    cp "$work/config/cover_generation.json" "$work/config-devices/"
    for file in global.py.json tweaks.json save_to_disk.py.json device_drivers_KOBOTOUCH.py.json \
        device_drivers_KINDLE2.py.json mtp_devices.json conversion; do
        [ -e "$user_config/$file" ] && cp -a "$user_config/$file" "$work/config-devices/"
    done
    local first again removed
    first=${picks% \[*}
    again=$(echo "$picks" | sed 's/.*\(\[[0-9]*\]\) .*/\1/')
    removed=${picks##* }
    CALIBRE_CONFIG_DIRECTORY="$work/config-devices" devices "$kind" "real-$kind" "$work/real-reliure" "$first" "$again" \
        "$removed"
    done
}

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
        real_conversions
        real_devices
    fi
fi

exit $status
