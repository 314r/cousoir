#!/usr/bin/env bash
# Builds tests/data/library-custom: tests/data/library with a custom column
# of each kind Calibre has, made and filled by calibredb, and
# tests/data/library-custom-list.json, what `calibredb list --for-machine
# --fields all` prints for it with the library's folder as @LIBRARY@.
#
# Calibre runs with a configuration of its own and a French interface, so the
# result does not depend on this computer's Calibre settings.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export CALIBRE_CONFIG_DIRECTORY="$work/config"
export CALIBRE_OVERRIDE_LANG=fr

library="$work/library-custom"
cp -a "$root/tests/data/library" "$library"
db() { calibredb --with-library "$library" "$@" >/dev/null; }

db add_custom_column genre Genre text --is-multiple
db add_custom_column people People text --is-multiple --display '{"is_names": true}'
db add_custom_column note Note text
db add_custom_column read Lu bool
db add_custom_column myrating "My rating" rating
db add_custom_column half Half rating --display '{"allow_half_stars": true}'
db add_custom_column pagecount Pages int
db add_custom_column price Price float
db add_custom_column started Started datetime
db add_custom_column review Review comments
db add_custom_column shelf Shelf enumeration --display '{"enum_values": ["A", "B", "C"]}'
db add_custom_column saga Saga series
db add_custom_column computed Computed composite --display '{"composite_template": "{title} by {authors}"}'

db set_custom genre 1 "SF,Classique"
db set_custom genre 2 SF
db set_custom genre 4 "Humour,Fantastique,SF"
db set_custom people 1 "Jean Dupont & Marie Curie"
db set_custom note 1 "Une note"
db set_custom note 3 "Ç'a été lu"
db set_custom read 1 true
db set_custom read 2 false
db set_custom myrating 1 8
db set_custom myrating 6 10
db set_custom half 2 7
db set_custom pagecount 1 320
db set_custom pagecount 4 0
db set_custom price 2 12.5
db set_custom price 3 3
db set_custom price 4 100000
db set_custom price 5 0.00001
db set_custom started 1 2024-03-05
db set_custom started 5 1999-12-31
db set_custom review 1 "<p>Bien</p>"
db set_custom shelf 3 B
db set_custom saga 1 Cycle
db set_custom saga 2 "Cycle [3.5]"
db set_custom saga 6 Autre

db backup_metadata --all
calibredb list --with-library "$library" --for-machine --fields all > "$work/list.json"

if [ -e "$library/metadata.db-journal" ]; then
    echo "Calibre left a journal behind" >&2
    exit 1
fi
rm -rf "$root/tests/data/library-custom"
cp -a "$library" "$root/tests/data/library-custom"
sed "s|$library|@LIBRARY@|g" "$work/list.json" > "$root/tests/data/library-custom-list.json"
echo "Wrote tests/data/library-custom and tests/data/library-custom-list.json"
