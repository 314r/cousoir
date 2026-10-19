#!/usr/bin/env python3
# Writes the EPUBs of tests/data/import, each with a package file that tries
# one corner of Calibre's OPF readers:
#
#   make_import_epubs.py <folder> <cover.jpg>

import os
import sys
import zipfile

folder, cover_path = sys.argv[1:3]
with open(cover_path, 'rb') as f:
    cover = f.read()

CONTAINER = '''<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="{missing}" media-type="application/oebps-package+xml"/>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
'''


def page(title, body=''):
    return f'''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>{title}</title></head>
<body><h1>{title}</h1>{body}</body>
</html>
'''


def write(name, opf, files, missing='OEBPS/missing.opf'):
    stamp = (2026, 1, 1, 0, 0, 0)
    with zipfile.ZipFile(os.path.join(folder, name), 'w') as epub:
        epub.writestr(zipfile.ZipInfo('mimetype', stamp), 'application/epub+zip', zipfile.ZIP_STORED)
        entries = [('META-INF/container.xml', CONTAINER.format(missing=missing)), ('OEBPS/content.opf', opf)]
        for entry, data in entries + list(files.items()):
            epub.writestr(zipfile.ZipInfo(entry, stamp), data, zipfile.ZIP_DEFLATED)


# EPUB 3: refines, a no-break space in a name, an editor, a subtitle, a
# collection, a fractional date, a cover-image item and identifiers.
write('Le Vicomte - Inconnu.epub', '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid"
         prefix="calibre: https://calibre-ebook.com">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:2a7d1c3e-8b2f-4a39-9e65-0f4c1d2b3a4f</dc:identifier>
    <dc:identifier id="isbn">urn:isbn:978-2-07-036024-5</dc:identifier>
    <dc:identifier>doi:10.1000/182</dc:identifier>
    <dc:title id="main">Le  Vicomte de Bragelonne</dc:title>
    <meta refines="#main" property="title-type">main</meta>
    <meta refines="#main" property="file-as">Vicomte de Bragelonne, Le</meta>
    <dc:title id="sub">Tome premier</dc:title>
    <meta refines="#sub" property="title-type">subtitle</meta>
    <dc:creator id="a1">Alexandre Dumas</dc:creator>
    <meta refines="#a1" property="role" scheme="marc:relators">aut</meta>
    <meta refines="#a1" property="file-as">Dumas, Alexandre</meta>
    <meta refines="#a1" property="display-seq">2</meta>
    <dc:creator id="a2">Auguste Maquet</dc:creator>
    <meta refines="#a2" property="role" scheme="marc:relators">aut</meta>
    <meta refines="#a2" property="display-seq">1</meta>
    <dc:creator id="e1">Jean Éditeur</dc:creator>
    <meta refines="#e1" property="role" scheme="marc:relators">edt</meta>
    <dc:contributor id="p1">Atelier Typographique</dc:contributor>
    <meta refines="#p1" property="role" scheme="marc:relators">bkp</meta>
    <dc:language>fr</dc:language>
    <dc:language>en</dc:language>
    <dc:date>1847-10-20T11:30:15.123456+01:00</dc:date>
    <dc:publisher>Michel Lévy</dc:publisher>
    <dc:publisher>Second Publisher</dc:publisher>
    <dc:subject>Roman historique</dc:subject>
    <dc:subject>  Cape   et épée </dc:subject>
    <dc:subject>roman historique</dc:subject>
    <dc:description>&lt;p&gt;Le dernier volet.&lt;/p&gt;</dc:description>
    <meta property="belongs-to-collection" id="c1">Les Mousquetaires</meta>
    <meta refines="#c1" property="collection-type">series</meta>
    <meta refines="#c1" property="group-position">3.5</meta>
    <meta property="calibre:rating">8</meta>
    <meta property="calibre:timestamp" scheme="dcterms:W3CDTF">2020-02-02T10:00:00+00:00</meta>
    <meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="text" href="text.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover" href="images/cover.jpg" media-type="image/jpeg" properties="cover-image"/>
  </manifest>
  <spine><itemref idref="text"/></spine>
</package>
''', {'OEBPS/nav.xhtml': page('Nav'), 'OEBPS/text.xhtml': page('Le Vicomte'), 'OEBPS/images/cover.jpg': cover})

# EPUB 2: opf:file-as and opf:role, Calibre's meta tags, an offset QTimeZone
# refuses, two dates, comma-separated subjects, a bare ISBN, and a cover
# meta whose percent-encoded href Calibre does not decode, so it renders the
# first page instead.
write('Black Company 2.epub', '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid_id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:identifier id="uuid_id" opf:scheme="uuid">0d3f2c1b-1111-4222-8333-944455556666</dc:identifier>
    <dc:identifier opf:scheme="calibre">0d3f2c1b-1111-4222-8333-944455556666</dc:identifier>
    <dc:identifier>9780812521399</dc:identifier>
    <dc:identifier opf:scheme="GOODREADS">40307</dc:identifier>
    <dc:title>Shadows   Linger</dc:title>
    <dc:creator opf:role="aut" opf:file-as="Cook, Glen">Glen Cook &amp; Someone Else</dc:creator>
    <dc:creator opf:role="ill">An Illustrator</dc:creator>
    <dc:contributor opf:role="bkp">calibre (9.16.0) [https://calibre-ebook.com]</dc:contributor>
    <dc:date>1984-05-20T19:59:00-17:00</dc:date>
    <dc:date opf:event="original-publication">1983-01-01</dc:date>
    <dc:language>eng</dc:language>
    <dc:subject>Fantasy, Military</dc:subject>
    <dc:subject>Dark fantasy</dc:subject>
    <dc:publisher>Tor</dc:publisher>
    <dc:description>The second book.</dc:description>
    <meta name="calibre:series" content="Chronicles of the Black Company"/>
    <meta name="calibre:series_index" content="2"/>
    <meta name="calibre:rating" content="6"/>
    <meta name="calibre:title_sort" content="Shadows Linger"/>
    <meta name="calibre:timestamp" content="2019-03-04T05:06:07+00:00"/>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
    <item id="text" href="text.html" media-type="application/xhtml+xml"/>
    <item id="cover-image" href="cover%20image.jpg" media-type="image/jpeg"/>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  </manifest>
  <spine toc="ncx"><itemref idref="text"/></spine>
</package>
''', {'OEBPS/text.html': page('Shadows Linger'), 'OEBPS/cover image.jpg': cover})

# No title and no author: the file's name gives them.
write('The Lost Title - Anne Onyme & Bob Builder.epub', '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:7b0c9d8e-2222-4333-8444-a55566667777</dc:identifier>
    <dc:language>und</dc:language>
  </metadata>
  <manifest><item id="text" href="text.html" media-type="application/xhtml+xml"/></manifest>
  <spine><itemref idref="text"/></spine>
</package>
''', {'OEBPS/text.html': page('Lost', '<p><img src="cover.jpg" alt="cover"/></p>'), 'OEBPS/cover.jpg': cover})

# A manga-style book whose first spine item is its cover image.
write('manga.epub', '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:9c8b7a6f-3333-4444-8555-b66677778888</dc:identifier>
    <dc:title>Manga</dc:title>
    <dc:creator>Mangaka</dc:creator>
    <dc:language>ja</dc:language>
  </metadata>
  <manifest>
    <item id="p1" href="p1.jpg" media-type="image/jpeg"/>
    <item id="text" href="text.html" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="p1"/><itemref idref="text"/></spine>
</package>
''', {'OEBPS/p1.jpg': cover, 'OEBPS/text.html': page('Manga')})
