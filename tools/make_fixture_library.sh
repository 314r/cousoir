#!/usr/bin/env bash
# Builds tests/data/library, a six-book library made by Calibre itself, and
# tests/data/library-list.json, what `calibredb list --for-machine --fields all`
# prints for it with the library's folder as @LIBRARY@. The books cover
# accents, a series, several authors, a fractional series index, a PDF, an
# AZW3, a book with no cover and one with no file.
#
# Ratings are given in stars; Calibre stores twice as many.
#
# Calibre runs with a configuration of its own and a French interface, so the
# result does not depend on this computer's Calibre settings.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export CALIBRE_CONFIG_DIRECTORY="$work/config"
export CALIBRE_OVERRIDE_LANG=fr

library="$work/library"
sources="$work/sources"
mkdir -p "$library" "$sources"
cd "$sources"

epub() { python3 "$root/tools/fixtures/make_epub.py" "$@"; }
cover() { magick -size 600x900 "xc:$2" -fill white -font "$(fc-match -f %{file} sans-serif)" -pointsize 54 -gravity center -annotate 0 "$3" "$1"; }
db() { calibredb --with-library "$library" "$@" >/dev/null; }

epub ecume.epub "L'Écume des jours" "Boris Vian" fr "Colin possède un appartement et un cuisinier."
epub black.epub "The Black Company" "Glen Cook" en "There were prodigies and portents enough."
epub shadows.epub "Shadows Linger" "Glen Cook" en "Marron Shed was a soft man."
epub omens.epub "Good Omens" "Terry Pratchett" en "In the beginning, it was a nice day."
python3 "$root/tools/fixtures/make_pdf.py" manuel.pdf "Manuel technique"
ebook-convert black.epub black.azw3 >/dev/null
cover ecume.jpg '#2f5d62' "L'Écume\ndes jours"
cover black.jpg '#3a3a3a' "The Black\nCompany"
cover shadows.jpg '#5b4636' "Shadows\nLinger"
cover omens.jpg '#7a2e2e' "Good\nOmens"
cover prince.jpg '#2e4a7a' "Le Petit\nPrince"

db add --title "L'Écume des jours" --authors "Boris Vian" --languages fra --tags "Roman,Classique" \
    --isbn 9782253140870 --cover ecume.jpg ecume.epub
db set_metadata 1 --field publisher:Gallimard --field rating:4 --field pubdate:1947-03-20 \
    --field 'comments:<div><p style="color:#000000">Un roman d’amour <font face="Arial">fou</font>.</p><p>Paris, <em>1946</em>.</p></div>'

db add --title "The Black Company" --authors "Glen Cook" --languages eng --tags Fantasy \
    --series "Chronicles of the Black Company" --series-index 1 --cover black.jpg black.epub
db add_format 2 black.azw3
db set_metadata 2 --field rating:5 --field pubdate:1984-05-01 --field publisher:Tor

db add --title "Shadows Linger" --authors "Glen Cook" --languages eng --tags Fantasy \
    --series "Chronicles of the Black Company" --series-index 2 --cover shadows.jpg shadows.epub

db add --title "Good Omens" --authors "Terry Pratchett & Neil Gaiman" --languages eng --tags "Fantasy,Humour" \
    --isbn 9780060853983 --identifier goodreads:12067 --cover omens.jpg omens.epub
db set_metadata 4 --field publisher:Gollancz --field rating:3.5 --field 'comments:The world will end next Saturday.'

# Added empty, as Calibre would make a cover from the PDF's first page.
db add --empty --title "Manuel technique" --authors "Jean-Pierre Dupont" --languages fra --tags Technique
db add_format 5 manuel.pdf

db add --empty --title "Le Petit Prince" --authors "Antoine de Saint-Exupéry" --languages fra \
    --series "Contes" --series-index 1.5 --cover prince.jpg

db backup_metadata --all
calibredb list --with-library "$library" --for-machine --fields all > "$work/list.json"

if [ -e "$library/metadata.db-journal" ]; then
    echo "Calibre left a journal behind" >&2
    exit 1
fi
rm -rf "$root/tests/data/library"
cp -a "$library" "$root/tests/data/library"
python3 - "$work/list.json" "$library" "$root/tests/data/library-list.json" <<'EOF'
import sys
raw, library, out = sys.argv[1:]
with open(raw, 'rb') as f:
    data = f.read()
with open(out, 'wb') as f:
    f.write(data.replace(library.encode(), b'@LIBRARY@'))
EOF
echo "Wrote tests/data/library and tests/data/library-list.json"
