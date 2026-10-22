#!/usr/bin/env bash
# Builds tests/data/import, book files that try the corners of Calibre's
# metadata readers: EPUB 2 and 3 package files, MOBI and AZW3 headers made by
# ebook-convert, PDF info dictionaries and XMP packets (one written by
# Calibre), covers to render and metadata only in a file's name.
# tests/golden/run.sh reads them with Cousoir and with Calibre.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
# cousoir-test-tool writes the book files; make fixtures builds it first.
tool=${COUSOIR_TEST_TOOL:-$root/build/cousoir-test-tool}
case $tool in /*) ;; *) tool=$PWD/$tool ;; esac
if [ ! -x "$tool" ]; then
    echo "$tool is missing; build it with make build" >&2
    exit 1
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export CALIBRE_CONFIG_DIRECTORY="$work/config"
export CALIBRE_OVERRIDE_LANG=fr

out="$root/tests/data/import"
rm -rf "$out"
mkdir -p "$out"

magick -size 400x600 'xc:#4a3b6b' -fill white -font "$(fc-match -f %{file} sans-serif)" -pointsize 40 \
    -gravity center -annotate 0 "Cover" -quality 85 "$work/cover.jpg"
"$tool" make-import-epubs "$out" "$work/cover.jpg"

# MOBI and AZW3: their EXTH headers as Calibre writes them.
ebook-convert "$out/Le Vicomte - Inconnu.epub" "$out/Vicomte.azw3" >/dev/null
ebook-convert "$out/Le Vicomte - Inconnu.epub" "$out/Vicomte.mobi" >/dev/null

# A PDF as LaTeX or Acrobat write them: an info dictionary and an XMP packet
# newer than it.
cat > "$work/info.json" <<'EOF'
{"Title": "Relativity Made Plain", "Author": "Ann Author and Bob Writer", "Subject": "Physics",
 "Keywords": "relativity, 978-0-306-40615-7, spacetime", "Creator": "LaTeX with hyperref",
 "Producer": "pdfTeX-1.40.25", "CreationDate": "D:20190401100000+02'00'", "ModDate": "D:20190401100000+02'00'"}
EOF
cat > "$work/packet.xml" <<'EOF'
<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/"
     xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmlns:prism="http://prismstandard.org/namespaces/basic/2.0/"
     xmlns:xmpidq="http://ns.adobe.com/xmp/Identifier/qual/1.0/">
   <dc:title><rdf:Alt><rdf:li xml:lang="en">English title</rdf:li><rdf:li xml:lang="x-default">Relativity, Plainly</rdf:li></rdf:Alt></dc:title>
   <dc:creator><rdf:Seq><rdf:li>Ann Author</rdf:li><rdf:li>Bob Writer &amp; Carl Coauthor</rdf:li></rdf:Seq></dc:creator>
   <dc:subject><rdf:Bag><rdf:li>Physics</rdf:li><rdf:li>Relativity</rdf:li><rdf:li>Physics</rdf:li></rdf:Bag></dc:subject>
   <dc:description><rdf:Alt><rdf:li xml:lang="x-default">A plain account.</rdf:li></rdf:Alt></dc:description>
   <dc:publisher><rdf:Bag><rdf:li>Plain Press</rdf:li></rdf:Bag></dc:publisher>
   <dc:language><rdf:Bag><rdf:li>en-US</rdf:li></rdf:Bag></dc:language>
   <xmp:CreateDate>2019-04-01T10:00:00+02:00</xmp:CreateDate>
   <xmp:ModifyDate>2019-04-03T09:00:00.25+02:00</xmp:ModifyDate>
   <xmp:MetadataDate>2019-04-02T09:00:00+02:00</xmp:MetadataDate>
   <xmp:CreatorTool>LaTeX</xmp:CreatorTool>
   <xmp:Identifier><rdf:Bag><rdf:li rdf:parseType="Resource"><xmpidq:Scheme>URL</xmpidq:Scheme><rdf:value>https://example.org/relativity</rdf:value></rdf:li></rdf:Bag></xmp:Identifier>
   <prism:doi>doi:10.1234/rel.2019</prism:doi>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>
EOF
"$tool" make-pdf "$out/relativity.pdf" "Relativity" "$work/info.json" "$work/packet.xml"

# The same, but the info dictionary changed after the packet: its title,
# authors and tags win.
sed -e "s/D:20190401100000+02'00'\", \"ModDate\": \"D:20190401100000+02'00'\"/D:20190401100000+02'00'\", \"ModDate\": \"D:20200101000000Z\"/" \
    "$work/info.json" > "$work/info-newer.json"
"$tool" make-pdf "$out/Newer Info - Someone.pdf" "Newer" "$work/info-newer.json" "$work/packet.xml"

# A PDF whose metadata Calibre wrote, XMP with its own namespace included.
"$tool" make-pdf "$out/calibre.pdf" "Calibre"
ebook-meta "$out/calibre.pdf" --title "Écrit par Calibre" --authors "Kovid Goyal & Anne Autre" \
    --tags "Logiciel,Livres" --series "Outils" --index 3 --isbn 9782070360246 --rating 4 \
    --comments "<p>Un PDF.</p>" --date 2001-02-03 --publisher "Éditions Libres" --language fr >/dev/null

# Books whose metadata Calibre wrote with custom columns' values, as
# calibredb export embeds them: an EPUB 3, an EPUB 2 and a PDF. The columns
# are those of tools/make_custom_library.sh but for a few, which adding the
# books to that library leaves out: People without names' separators, Half
# a number, Shelf with a value more, Elsewhere none of its columns.
custom="$work/custom"
mkdir -p "$custom"
db() { calibredb --with-library "$custom" "$@" >/dev/null; }
db add_custom_column genre Genre text --is-multiple
db add_custom_column people People text --is-multiple
db add_custom_column note Note text
db add_custom_column read Lu bool
db add_custom_column myrating "My rating" rating
db add_custom_column half Half int
db add_custom_column pagecount Pages int
db add_custom_column price Price float
db add_custom_column started Started datetime
db add_custom_column review Review comments
db add_custom_column shelf Shelf enumeration --display '{"enum_values": ["A", "B", "C", "D"]}'
db add_custom_column saga Saga series
db add_custom_column elsewhere Elsewhere text
db add --duplicates "$out/Le Vicomte - Inconnu.epub"
db add --duplicates "$out/Black Company 2.epub"
db add --duplicates "$out/calibre.pdf"
db set_custom genre 1 "SF,Classique"
db set_custom people 1 "Jean Dupont,Marie Curie"
db set_custom note 1 "Une note"
db set_custom read 1 true
db set_custom myrating 1 8
db set_custom half 1 7
db set_custom pagecount 1 320
db set_custom price 1 12.5
db set_custom started 1 2024-03-05
db set_custom review 1 "<p>Bien</p>"
db set_custom shelf 1 D
db set_custom saga 1 "Cycle [3.5]"
db set_custom elsewhere 1 "Ailleurs"
db set_custom genre 2 Humour
db set_custom note 2 "Autre note"
db set_custom read 2 false
db set_custom myrating 2 4
db set_custom pagecount 2 0
db set_custom price 2 3
db set_custom started 2 1999-12-31
db set_custom shelf 2 B
db set_custom saga 2 Autre
db set_custom genre 3 SF
db set_custom note 3 "Note du PDF"
db set_custom read 3 true
db set_custom pagecount 3 12
db set_custom shelf 3 A
db set_custom saga 3 "Cycle [2]"
for book in 1 2 3; do
    calibredb --with-library "$custom" export "$book" --to-dir "$work/export-$book" --single-dir >/dev/null
done
cp "$(find "$work/export-1" -name '*.epub')" "$out/Custom 3.epub"
cp "$(find "$work/export-2" -name '*.epub')" "$out/Custom 2.epub"
cp "$(find "$work/export-3" -name '*.pdf')" "$out/Custom.pdf"

# Formats Cousoir leaves to Calibre, and one Calibre has no reader for.
printf 'Chapter one.\n' > "$out/Plain Notes - Jane Doe.txt"
printf 'nothing to read\n' > "$out/data.xyz"
