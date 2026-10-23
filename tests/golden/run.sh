#!/usr/bin/env bash
# Compares `cousoir-cli list` with `calibredb list --for-machine --fields all`,
# each on its own copy of the fixture library and, with COUSOIR_GOLDEN_REAL=1,
# of the library Calibre opens; `cousoir-cli read` with Calibre's readers on
# book files; and the libraries Calibre and `cousoir-cli apply` leave after
# the same edits, additions, removals, conversions and metadata downloads.
# calibredb can rename files and touches metadata.db even to list it, so
# neither ever runs on the library itself.
#
#   tests/golden/run.sh build/cousoir-cli [build/cousoir-test-tool]
#
# cousoir-test-tool, beside cousoir-cli unless given, does the comparing and
# what the fake readers do between steps. Exits 77, which ctest counts as
# skipped, without calibredb.
set -euo pipefail

cli=$1
tool=${2:-$(dirname "$cli")/cousoir-test-tool}
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! command -v calibredb >/dev/null; then
    echo "calibredb is not installed; skipping"
    exit 77
fi
if [ ! -x "$tool" ]; then
    echo "$tool is missing; build it with make build"
    exit 1
fi

# Under the home folder by default: on a copy-on-write filesystem the copies
# of a large library then cost nothing.
base=${COUSOIR_GOLDEN_TMP:-${XDG_CACHE_HOME:-$HOME/.cache}/cousoir-golden}
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"' EXIT
# calibredb keeps its own preferences away from the user's, and cousoir-cli
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
    for side in calibre cousoir; do
        cp -a --reflink=auto "$source" "$work/$name-$side"
        # Dropbox conflict copies are not part of the library.
        find "$work/$name-$side" -maxdepth 1 -type f \
            \( -iname '*conflicted copy*' -o -iname '*copie en conflit*' -o -iname '*case conflict*' \) -delete
    done
    calibredb list --with-library "$work/$name-calibre" --for-machine --fields all \
        | sed -e "s|$work/$name-calibre|@LIBRARY@|g" > "$work/$name-calibre.json"
    "$cli" list --library "$work/$name-cousoir" \
        | sed "s|$work/$name-cousoir|@LIBRARY@|g" > "$work/$name-cousoir.json"
    if cmp -s "$work/$name-calibre.json" "$work/$name-cousoir.json"; then
        echo "$name: identical ($(grep -c '"id":' "$work/$name-calibre.json") books)"
    else
        echo "$name: cousoir-cli list differs from calibredb list"
        diff -u "$work/$name-calibre.json" "$work/$name-cousoir.json" | head -60
        status=1
    fi
}

compare fixture "$root/tests/data/library"
compare custom "$root/tests/data/library-custom"
# Two thousand made-up books, for the names, titles and files six books
# leave out: names in other scripts, long titles, the same book twice.
"$tool" make-library "$work/generated" 2000 7 > /dev/null
compare generated "$work/generated"

# What adding a file reads from it, Calibre's readers against Cousoir's, on
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
    if ! "$cli" read "${files[@]}" > "$work/$name-read-cousoir.json" 2> "$work/$name-read.log"; then
        echo "$name reads: cousoir-cli read failed:"
        tail -20 "$work/$name-read.log"
        status=1
        return 0
    fi
    if "$tool" compare-reads "$work/$name-read-calibre.json" "$work/$name-read-cousoir.json" \
        > "$work/$name-reads.txt"; then
        echo "$name reads: $(tail -1 "$work/$name-reads.txt")"
    else
        echo "$name reads: Cousoir reads files differently:"
        head -60 "$work/$name-reads.txt"
        status=1
    fi
}

reads fixture "$root/tests/data/import" "$root/tests/data/library"

# What the templates of tests/data/templates/templates.json make of each book,
# with Calibre's formatter (tools/oracle/template_oracle.py) and Cousoir's:
# the same text, or an error on both sides.
templates() {
    local name=$1 source=$2
    rm -rf "$work/$name-templates"
    cp -a --reflink=auto "$source" "$work/$name-templates"
    oracle "$root/tools/oracle/template_oracle.py" -- "$work/$name-templates" \
        "$root/tests/data/templates/templates.json" "$work/$name-templates-calibre.json" || return 0
    if ! "$cli" template --library "$work/$name-templates" "$root/tests/data/templates/templates.json" \
        > "$work/$name-templates-cousoir.json" 2> "$work/$name-templates.log"; then
        echo "$name templates: cousoir-cli template failed:"
        tail -20 "$work/$name-templates.log"
        status=1
        return 0
    fi
    if "$tool" compare-templates "$work/$name-templates-calibre.json" "$work/$name-templates-cousoir.json" \
        > "$work/$name-templates.txt"; then
        echo "$name templates: $(tail -1 "$work/$name-templates.txt")"
    else
        echo "$name templates: Cousoir's templates differ from Calibre's:"
        head -60 "$work/$name-templates.txt"
        status=1
    fi
}

templates fixture "$root/tests/data/library"
templates custom "$root/tests/data/library-custom"
templates generated "$work/generated"

# The searches of tests/data/search/queries.txt, Calibre's and Cousoir's, on
# a library with the saved searches, virtual libraries, grouped search terms
# and user categories of tests/data/search/preferences.sql added.
searches() {
    local name=$1 source=$2
    rm -rf "$work/$name-search"
    cp -a --reflink=auto "$source" "$work/$name-search"
    "$tool" run-sql "$work/$name-search/metadata.db" "$root/tests/data/search/preferences.sql"
    oracle "$root/tools/oracle/search_books.py" -- "$work/$name-search" "$root/tests/data/search/queries.txt" \
        "$work/$name-search-calibre.json" || return 0
    "$tool" search-books "$work/$name-search" "$root/tests/data/search/queries.txt" "$work/$name-search-cousoir.json"
    if "$tool" compare-searches "$work/$name-search-calibre.json" "$work/$name-search-cousoir.json" \
        > "$work/$name-searches.txt"; then
        echo "$name searches: $(tail -1 "$work/$name-searches.txt")"
    else
        echo "$name searches: Cousoir finds other books than Calibre:"
        head -60 "$work/$name-searches.txt"
        status=1
    fi
}

searches fixture "$root/tests/data/library"
searches custom "$root/tests/data/library-custom"
searches generated "$work/generated"

# The sorts of tests/data/sort/sorts.json, sort histories as Calibre's list
# keeps them, through Calibre's multisort() and Cousoir's, on the three
# libraries.
sorts() {
    local name=$1 source=$2
    rm -rf "$work/$name-sort"
    cp -a --reflink=auto "$source" "$work/$name-sort"
    oracle "$root/tools/oracle/sort_oracle.py" -- "$work/$name-sort" \
        "$root/tests/data/sort/sorts.json" "$work/$name-sort-calibre.json" || return 0
    "$tool" sort-books "$work/$name-sort" "$root/tests/data/sort/sorts.json" \
        "$work/$name-sort-cousoir.json"
    if "$tool" compare-sorts "$work/$name-sort-calibre.json" "$work/$name-sort-cousoir.json" > "$work/$name-sorts.txt"; then
        echo "$name sorts: $(tail -1 "$work/$name-sorts.txt")"
    else
        echo "$name sorts: Cousoir sorts the books otherwise than Calibre:"
        grep -v "not compared" "$work/$name-sorts.txt" | head -40
        status=1
    fi
}

sorts fixture "$root/tests/data/library"
sorts custom "$root/tests/data/library-custom"
sorts generated "$work/generated"

# The same edits written by Calibre and by Cousoir, then compared table by
# table and file by file; calibredb then checks Cousoir's copy and lists it as
# it lists Calibre's.
edits() {
    local edits=$1 library=${2:-$root/tests/data/library}
    for side in calibre cousoir; do
        rm -rf "$work/edit-$side"
        cp -a --reflink=auto "$library" "$work/edit-$side"
    done
    oracle "$root/tools/oracle/apply_edits.py" -- "$work/edit-calibre" "$edits" || return 0
    if ! "$cli" apply --force --library "$work/edit-cousoir" "$edits" >/dev/null; then
        status=1
        return 0
    fi
    if ! "$tool" compare-libraries "$work/edit-calibre" "$work/edit-cousoir"; then
        status=1
    fi
    # last_modified is the time of each side's write, a second apart at times.
    for side in calibre cousoir; do
        calibredb check_library --with-library "$work/edit-$side" > "$work/check-$side.txt" 2>&1 || true
        calibredb list --with-library "$work/edit-$side" --for-machine --fields all \
            | sed -e "s|$work/edit-$side|@LIBRARY@|g" -e 's|"last_modified": "[^"]*"|"last_modified": "<now>"|' \
            > "$work/list-$side.json"
    done
    if ! cmp -s "$work/check-calibre.txt" "$work/check-cousoir.txt"; then
        echo "calibredb check_library reports differently:"
        diff -u "$work/check-calibre.txt" "$work/check-cousoir.txt" | head -30
        status=1
    fi
    if ! cmp -s "$work/list-calibre.json" "$work/list-cousoir.json"; then
        echo "calibredb lists the copies differently:"
        diff -u "$work/list-calibre.json" "$work/list-cousoir.json" | head -40
        status=1
    else
        echo "edits: calibredb lists both copies the same"
    fi
}

edits "$root/tests/data/edits/edits.json"
edits "$root/tests/data/edits/bulk.json"
edits "$root/tests/data/edits/custom.json" "$root/tests/data/library-custom"

# Metadata downloads, without the web: Cousoir's identify job run on the
# books of found.json as Calibre's sources would return them, then the books
# found applied to copies of the fixture by Calibre's Edit metadata dialog,
# shown offscreen, and by Cousoir.
downloads() {
    local dir="$work/download"
    rm -rf "$dir" "$work/download-calibre" "$work/download-cousoir"
    mkdir -p "$dir"
    cp "$root/tests/data/download/download.json" "$dir/"
    oracle "$root/tools/oracle/download_oracle.py" -- identify "$root/metadata_download.cpp" "$dir/found" \
        "$root/tests/data/download/found.json" || return 0
    cp -a "$dir/found" "$dir/covers"
    oracle "$root/tools/oracle/download_oracle.py" -- covers "$root/metadata_download.cpp" "$dir/covers" || return 0
    for side in calibre cousoir; do
        cp -a --reflink=auto "$root/tests/data/library" "$work/download-$side"
    done
    QT_QPA_PLATFORM=offscreen oracle "$root/tools/oracle/download_oracle.py" -- apply "$work/download-calibre" \
        "$dir/download.json" || return 0
    if ! "$cli" apply --force --library "$work/download-cousoir" "$dir/download.json" >/dev/null; then
        status=1
        return 0
    fi
    if "$tool" compare-libraries "$work/download-calibre" "$work/download-cousoir"; then
        echo "downloads: Calibre's dialog and Cousoir change the books alike"
    else
        status=1
    fi
}

downloads

# Books and formats added, moved to the trash and restored, then one copy of
# a book kept with the formats of another, step by step on both copies of
# LIBRARY, each step compared. PREPARE, if not empty, runs on each copy
# first.
operations() {
    local library=$1 prepare=$2
    shift 2
    for side in calibre cousoir; do
        rm -rf "$work/ops-$side"
        cp -a --reflink=auto "$library" "$work/ops-$side"
        if [ -n "$prepare" ]; then
            "$prepare" "$work/ops-$side"
        fi
    done
    local ops
    for ops in "$@"; do
        echo "$(basename "$ops"):"
        oracle "$root/tools/oracle/apply_edits.py" -- "$work/ops-calibre" "$ops" || return 0
        if ! "$cli" apply --force --library "$work/ops-cousoir" "$ops" >/dev/null; then
            status=1
            return
        fi
        if ! "$tool" compare-libraries --added-from 7 "$work/ops-calibre" "$work/ops-cousoir"; then
            status=1
        fi
        for side in calibre cousoir; do
            calibredb check_library --with-library "$work/ops-$side" > "$work/ops-check-$side.txt" 2>&1 || true
        done
        if ! cmp -s "$work/ops-check-calibre.txt" "$work/ops-check-cousoir.txt"; then
            echo "calibredb check_library reports differently:"
            diff -u "$work/ops-check-calibre.txt" "$work/ops-check-cousoir.txt" | head -30
            status=1
        fi
        for side in calibre cousoir; do
            cp -a --reflink=auto "$work/ops-$side" "$work/after-$(basename "$ops" .json)-$side"
        done
    done
}

operations "$root/tests/data/library" "" "$root/tests/data/edits/ops-1.json" "$root/tests/data/edits/ops-2.json" \
    "$root/tests/data/edits/ops-3.json" "$root/tests/data/edits/ops-4.json"

# Each restores what the other moved to the trash: the step SECOND applied by
# Calibre to Cousoir's copy after FIRST, and by Cousoir to Calibre's, ends as
# the other.
cross() {
    local first=$1 second=$2
    rm -rf "$work/cross-calibre" "$work/cross-cousoir"
    cp -a --reflink=auto "$work/after-$first-cousoir" "$work/cross-calibre"
    cp -a --reflink=auto "$work/after-$first-calibre" "$work/cross-cousoir"
    oracle "$root/tools/oracle/apply_edits.py" -- "$work/cross-calibre" "$root/tests/data/edits/$second.json" \
        || return 0
    if ! "$cli" apply --force --library "$work/cross-cousoir" "$root/tests/data/edits/$second.json" >/dev/null; then
        status=1
        return 0
    fi
    echo "Calibre restoring Cousoir's trash:"
    "$tool" compare-libraries --added-from 7 "$work/after-$second-calibre" "$work/cross-calibre" \
        || status=1
    echo "Cousoir restoring Calibre's trash:"
    "$tool" compare-libraries --added-from 7 "$work/after-$second-cousoir" "$work/cross-cousoir" \
        || status=1
}

cross ops-1 ops-2

# The same on custom columns: books added with the values their files hold
# and those the columns give new books, then restored with theirs.
custom_defaults() {
    "$tool" run-sql "$1/metadata.db" "$root/tests/data/edits/defaults.sql"
}
operations "$root/tests/data/library-custom" custom_defaults "$root/tests/data/edits/ops-custom-1.json" \
    "$root/tests/data/edits/ops-custom-2.json"
cross ops-custom-1 ops-custom-2

# Books converted by Calibre's convert action and by cousoir-cli, compared:
# the files of conversions by their content, without the time each stamps
# or the ids it draws. `prepare`, if given, runs on each copy first.
conversions() {
    local name=$1 source=$2 ops=$3 converted=$4 prepare=${5:-}
    for side in calibre cousoir; do
        rm -rf "$work/$name-$side"
        cp -a --reflink=auto "$source" "$work/$name-$side"
        if [ -n "$prepare" ]; then
            "$prepare" "$work/$name-$side"
        fi
    done
    oracle "$root/tools/oracle/convert_books.py" -- "$work/$name-calibre" "$ops" || return 0
    if ! "$cli" apply --force --library "$work/$name-cousoir" "$ops" > "$work/$name.log" 2>&1; then
        echo "$name: cousoir-cli apply failed:"
        tail -20 "$work/$name.log"
        status=1
        return 0
    fi
    if "$tool" compare-libraries --converted "$converted" "$work/$name-calibre" \
        "$work/$name-cousoir" > "$work/$name.txt"; then
        echo "$name: $(tail -1 "$work/$name.txt")"
    else
        echo "$name: Cousoir converts differently:"
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
# Some books keep the settings of earlier conversions.
conversion_settings() { "$tool" conversion-settings "$1"; }
conversions conversions "$root/tests/data/library" "$root/tests/data/edits/convert.json" 1:KEPUB,2:AZW3,3:AZW3,5:KEPUB \
    conversion_settings

# The round trip, on the custom library edited last: Calibre writes again the
# metadata.opf files of the books Cousoir changed, as its GUI does when it
# starts, and finds nothing to change in them, custom columns included; then
# edits the library itself, which Cousoir reads as calibredb does.
round_trip() {
    rm -rf "$work/trip" "$work/trip-before"
    cp -a --reflink=auto "$work/edit-cousoir" "$work/trip"
    cp -a --reflink=auto "$work/trip" "$work/trip-before"
    calibredb backup_metadata --with-library "$work/trip" >/dev/null
    if ! "$tool" compare-libraries --files-only "$work/trip" "$work/trip-before" >/dev/null; then
        echo "round trip: Calibre writes the metadata.opf files differently:"
        "$tool" compare-libraries --files-only "$work/trip" "$work/trip-before" | head -20
        status=1
    fi
    calibredb set_metadata --with-library "$work/trip" 1 --field "title:Mood Indigo" --field "tags:Jazz,Roman" >/dev/null
    calibredb list --with-library "$work/trip" --for-machine --fields all \
        | sed -e "s|$work/trip|@LIBRARY@|g" > "$work/trip-calibre.json"
    "$cli" list --library "$work/trip" | sed "s|$work/trip|@LIBRARY@|g" > "$work/trip-cousoir.json"
    if cmp -s "$work/trip-calibre.json" "$work/trip-cousoir.json"; then
        echo "round trip: Calibre keeps Cousoir's files and Cousoir reads Calibre's change"
    else
        echo "round trip: Cousoir reads Calibre's change differently:"
        diff -u "$work/trip-calibre.json" "$work/trip-cousoir.json" | head -30
        status=1
    fi
}

round_trip

# Books sent to a reader and removed from it by Calibre's GUI and by
# cousoir-cli, each on a fake reader of its own, a Kobo or a Kindle reached
# over MTP, step by step: some sent, more sent once the reader had them a
# while, one of them again, one removed. Between steps, a Kobo takes the
# books in its database (cousoir-test-tool kobo-import) and Amazon blanks a
# Kindle's covers (cousoir-test-tool kindle-sync), which both programs put
# back. Removing, Cousoir leaves the other books' entries of
# metadata.calibre as they are, which Calibre writes again whenever it
# connects: only which books remain is compared then, and both readers once
# Calibre has connected to them. Then each program reads the other's reader:
# Calibre must take the books Cousoir sent for the library's, by their uuid,
# and Cousoir Calibre's.
#
#   devices <kobo|kindle> <name> <library> <books first> <books then> <book removed>
devices() {
    local kind=$1 name=$2 library=$3 side step lpath product=0 calibre_device cousoir_device native=()
    # Cousoir sending without Calibre: its KEPUBs are its own.
    [ -n "${COUSOIR_NATIVE_SEND:-}" ] && native=(--native)
    for side in calibre cousoir; do
        rm -rf "$work/$name-library-$side" "$work/$name-$side"
        cp -a --reflink=auto "$library" "$work/$name-library-$side"
        if [ "$kind" = kobo ]; then
            mkdir -p "$work/$name-$side/.kobo"
            printf 'N418000000000,4.1.15,4.38.21908,4.1.15,4.1.15,00000000-0000-0000-0000-000000000390' \
                > "$work/$name-$side/.kobo/version"
            "$tool" run-sql "$work/$name-$side/.kobo/KoboReader.sqlite" "$root/tests/data/kobo.sql"
        else
            mkdir -p "$work/$name-$side/documents" "$work/$name-$side/system/thumbnails"
        fi
    done
    calibre_device="$work/$name-calibre"
    cousoir_device="$work/$name-cousoir"
    if [ "$kind" = kindle ]; then
        product=0x9981
        calibre_device="mtpdir:$calibre_device"
        cousoir_device="mtpdir:$cousoir_device"
    fi
    for step in 1 2 3; do
        if [ "$step" -lt 3 ]; then
            local books=$4
            [ "$step" = 2 ] && books=$5
            printf '[{"send": %s}]' "$books" > "$work/$name-ops-calibre.json"
            printf '[{"send": {"books": %s, "device": "%s"}}]' "$books" "$cousoir_device" \
                > "$work/$name-ops-cousoir.json"
        else
            # The book's file as Calibre named it on its reader.
            lpath=$("$tool" device-lpath "$work/$name-calibre/metadata.calibre" "$6")
            printf '[{"remove": %s}]' "$lpath" > "$work/$name-ops-calibre.json"
            printf '[{"removeFromDevice": {"lpaths": %s, "device": "%s"}}]' "$lpath" "$cousoir_device" \
                > "$work/$name-ops-cousoir.json"
        fi
        oracle "$root/tools/oracle/send_books.py" -- "$work/$name-library-calibre" "$calibre_device" "$product" \
            "$work/$name-ops-calibre.json" || return 0
        if ! "$cli" apply --library "$work/$name-library-cousoir" "$work/$name-ops-cousoir.json" > "$work/$name.log" 2>&1; then
            echo "$name step $step: cousoir-cli apply failed:"
            tail -20 "$work/$name.log"
            status=1
            return 0
        fi
        local only=()
        [ "$step" = 3 ] && only=(--lpaths-only)
        if "$tool" compare-devices "${only[@]}" "${native[@]}" "$work/$name-calibre" "$work/$name-cousoir" \
            > "$work/$name.txt"; then
            echo "$name step $step: $(tail -1 "$work/$name.txt")"
        else
            echo "$name step $step: Cousoir sends differently:"
            head -40 "$work/$name.txt"
            status=1
        fi
        for side in calibre cousoir; do
            if [ "$kind" = kobo ]; then
                "$tool" kobo-import "$work/$name-$side"
            else
                "$tool" kindle-sync "$work/$name-$side"
            fi
        done
    done
    # Each reader read by both programs, on copies.
    for side in calibre cousoir; do
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
        "$cli" device --library "$work/$name-library-cousoir" "$listed" | "$tool" device-books \
            > "$work/$name-listed-$side.json"
    done
    if "$tool" compare-devices "${native[@]}" "$work/$name-read-calibre" "$work/$name-read-cousoir" \
        > "$work/$name.txt"; then
        echo "$name once Calibre connects: $(tail -1 "$work/$name.txt")"
    else
        echo "$name once Calibre connects: the readers differ:"
        head -40 "$work/$name.txt"
        status=1
    fi
    if ! "$tool" check-matches "$work/$name-matches-calibre.json" "$work/$name-matches-cousoir.json" \
        || ! cmp -s "$work/$name-matches-calibre.json" "$work/$name-matches-cousoir.json"; then
        echo "$name: Calibre does not take the books on both readers for the library's alike:"
        diff -u "$work/$name-matches-calibre.json" "$work/$name-matches-cousoir.json" | head -20
        status=1
    elif ! cmp -s "$work/$name-listed-calibre.json" "$work/$name-listed-cousoir.json" \
        || grep -q '"book": 0' "$work/$name-listed-calibre.json"; then
        echo "$name: Cousoir does not list the books on both readers alike:"
        diff -u "$work/$name-listed-calibre.json" "$work/$name-listed-cousoir.json" | head -20
        status=1
    else
        echo "$name: Calibre and Cousoir each know the books the other sent ($(grep -c '"lpath"' "$work/$name-listed-calibre.json") books)"
    fi
}

devices kobo kobo "$root/tests/data/library" '[1, 2, 3, 5]' '[2, 4]' 3
# The same, Cousoir sending without Calibre.
COUSOIR_NATIVE_SEND=1 devices kobo kobo-native "$root/tests/data/library" '[1, 2, 3, 5]' '[2, 4]' 3
devices kindle kindle "$root/tests/data/library" '[2, 5]' '[2]' 5
COUSOIR_NATIVE_SEND=1 devices kindle kindle-native "$root/tests/data/library" '[2, 5]' '[2]' 5

# Notes cousoir-cli writes, changes and removes on a copy of the fixture
# with the notes Calibre wrote (tests/data/notes), then read, searched,
# changed and brought back by Calibre itself (tools/oracle/notes_check.py):
# the notes' full text index must be as Calibre's own tokenizer makes it.
notes() {
    local library="$work/notes-library"
    rm -rf "$library"
    cp -a --reflink=auto "$root/tests/data/library" "$library"
    rm -rf "$library/.calnotes"
    cp -a "$root/tests/data/notes/.calnotes" "$library/.calnotes"
    cat > "$work/notes-ops.json" <<'JSON'
[{"note": {"field": "tags", "item": 3, "name": "Fantasy", "html": "<p>Other worlds, élégantes.</p>", "text": "Other worlds, élégantes."}},
 {"note": {"field": "publisher", "item": 2, "name": "Tor", "html": "<p>New York.</p>", "text": "New York."}},
 {"note": {"field": "series", "item": 2, "name": "Contes", "html": ""}}]
JSON
    if ! "$cli" apply --library "$library" "$work/notes-ops.json" > "$work/notes.log" 2>&1; then
        echo "notes: cousoir-cli apply failed:"
        tail -5 "$work/notes.log"
        status=1
        return 0
    fi
    if oracle "$root/tools/oracle/notes_check.py" -- "$library" > "$work/notes-check.txt"; then
        echo "notes: Calibre reads, searches, changes and brings back the notes Cousoir wrote"
    else
        echo "notes: Calibre and Cousoir's notes differ:"
        cat "$work/notes-check.txt"
        status=1
    fi
}

notes

# A Kobo's highlights of The Voyage, synced into a copy of the library by
# cousoir-cli: Calibre lists, searches and changes them, and a second sync
# leaves what it changed.
kobo_highlights() {
    local library="$work/highlights-library" kobo="$work/highlights-kobo"
    rm -rf "$library" "$kobo"
    cp -a --reflink=auto "$root/tests/data/library" "$library"
    mkdir -p "$kobo/.kobo"
    printf 'N418000000000,4.1.15,4.38.21908,4.1.15,4.1.15,00000000-0000-0000-0000-000000000390' > "$kobo/.kobo/version"
    "$tool" run-sql "$kobo/.kobo/KoboReader.sqlite" "$root/tests/data/kobo.sql"
    "$tool" run-sql "$kobo/.kobo/KoboReader.sqlite" "$root/tests/data/kobo-highlights.sql"
    cp "$root/tests/data/reader/voyage.epub" "$work/voyage.epub"
    printf '[{"add": "voyage.epub"}]' > "$work/highlights-add.json"
    printf '[{"syncHighlights": {"device": "highlights-kobo"}}]' > "$work/highlights-sync.json"
    if ! "$cli" apply --force --library "$library" "$work/highlights-add.json" > "$work/highlights.log" 2>&1 \
        || ! "$cli" apply --force --library "$library" "$work/highlights-sync.json" >> "$work/highlights.log" 2>&1; then
        echo "highlights: cousoir-cli apply failed:"
        tail -5 "$work/highlights.log"
        status=1
        return 0
    fi
    if ! oracle "$root/tools/oracle/kobo_highlights_check.py" -- "$library" synced > "$work/highlights-check.txt"; then
        echo "highlights: Calibre and Cousoir's Kobo highlights differ:"
        cat "$work/highlights-check.txt"
        return 0
    fi
    if ! "$cli" apply --force --library "$library" "$work/highlights-sync.json" >> "$work/highlights.log" 2>&1; then
        echo "highlights: cousoir-cli apply failed the second time:"
        tail -5 "$work/highlights.log"
        status=1
        return 0
    fi
    if oracle "$root/tools/oracle/kobo_highlights_check.py" -- "$library" again > "$work/highlights-check.txt"; then
        echo "highlights: Calibre reads, searches and changes the Kobo's highlights Cousoir put in, which keeps its changes"
    else
        echo "highlights: a second sync undid Calibre's changes:"
        cat "$work/highlights-check.txt"
        status=1
    fi
}
kobo_highlights

# What a Kobo knows of the reading of its books, read by Calibre's Kobo
# Bookmark (tools/oracle/kobo_reading.py) and by `cousoir-cli device`, on a
# fake Kobo whose database has books read, finished and bought, with
# highlights, notes and bookmarks (tests/data/kobo-reading.sql).
reading() {
    local kobo="$work/reading-kobo"
    rm -rf "$kobo"
    mkdir -p "$kobo/.kobo" "$kobo/Vian, Boris" "$kobo/Pratchett, Terry"
    printf 'N418000000000,4.1.15,4.38.21908,4.1.15,4.1.15,00000000-0000-0000-0000-000000000390' > "$kobo/.kobo/version"
    "$tool" run-sql "$kobo/.kobo/KoboReader.sqlite" "$root/tests/data/kobo.sql"
    "$tool" run-sql "$kobo/.kobo/KoboReader.sqlite" "$root/tests/data/kobo-reading.sql"
    for file in "Vian, Boris/Ecume.kepub.epub" "Pratchett, Terry/Mort.epub" Notes.pdf; do
        printf 'book' > "$kobo/$file"
    done
    oracle "$root/tools/oracle/kobo_reading.py" -- "$kobo" "$work/reading-calibre.json" || return 0
    if ! "$cli" device --library "$root/tests/data/library" "$kobo" > "$work/reading-cousoir.json"; then
        echo "reading: cousoir-cli device failed"
        status=1
        return 0
    fi
    if "$tool" compare-reading "$work/reading-calibre.json" "$work/reading-cousoir.json" > "$work/reading.txt"; then
        echo "reading: $(tail -1 "$work/reading.txt")"
    else
        echo "reading: Cousoir reads the Kobo's reading differently:"
        head -40 "$work/reading.txt"
        status=1
    fi
}

reading

# A few books of the real library converted, with the user's Calibre
# settings: the two smallest EPUBs to KEPUB and AZW3, the smallest book
# with a KEPUB to KEPUB again, and the smallest whose only source is an AZW3
# to AZW3, which keeps its file as ORIGINAL_AZW3; one Calibre converted
# before, so not locked by DRM.
real_conversions() {
    local converted
    converted=$("$tool" pick-conversions "$work/real-cousoir/metadata.db" "$work/convert-real.json")
    # The user's conversion settings, without the rest of Calibre's.
    rm -rf "$work/config-real"
    mkdir -p "$work/config-real"
    cp "$work/config/cover_generation.json" "$work/config-real/"
    for file in global.py.json tweaks.json conversion; do
        [ -e "$user_config/$file" ] && cp -a "$user_config/$file" "$work/config-real/"
    done
    CALIBRE_CONFIG_DIRECTORY="$work/config-real" conversions real-conversions "$work/real-cousoir" \
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
    picks=$("$tool" pick-sends "$work/real-cousoir/metadata.db" "$kind")
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
    CALIBRE_CONFIG_DIRECTORY="$work/config-devices" devices "$kind" "real-$kind" "$work/real-cousoir" "$first" "$again" \
        "$removed"
    done
}

if [ "${COUSOIR_GOLDEN_REAL:-0}" = 1 ]; then
    real=$("$tool" json-get "$user_config/global.py.json" library_path)
    if [ -z "$real" ]; then
        echo "Calibre has no library on this computer"
        status=1
    elif grep -q "@calibre-singleinstance-$(id -u)-db" /proc/net/unix 2>/dev/null; then
        echo "Calibre is open; close it so the copy is consistent"
        status=1
    else
        compare real "$real"
        reads real "$work/real-cousoir"
        real_conversions
        real_devices
    fi
fi

exit $status
