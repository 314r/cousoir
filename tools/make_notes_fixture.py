#!/usr/bin/env python3
# Writes tests/data/reader/notes.epub, a book for the reader's notes and
# pictures: EPUB 3 noterefs to an aside of the same page, Gutenberg's
# bracketed numbers and a raised number to a page of notes, a link that is
# no note, and a picture.
#
#   python3 tools/make_notes_fixture.py tests/data/reader/notes.epub

import struct
import sys
import zipfile
import zlib


def png(width, height):
    # Bands of colour, for a picture that shows when zoomed.
    rows = b''
    for y in range(height):
        band = (y * 4 // height)
        colour = [(200, 60, 60), (60, 160, 80), (60, 90, 200), (230, 200, 60)][band]
        rows += b'\x00' + bytes(colour) * width
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def page(title, body):
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en">'
            f'<head><title>{title}</title></head>\n<body>\n{body}\n</body></html>\n')


filler = ' '.join(['The harbour was quiet and the lanterns burned low over the water.'] * 6)
chapter1 = page('Notes', f'''<h1 id="top">Notes</h1>
<p>A word with a footnote<a epub:type="noteref" id="r1" href="#fn1">1</a> in the same page.</p>
<p>A word with an endnote<a id="ref2" href="notes.xhtml#note2">[2]</a> in another page, and a raised one<sup><a href="notes.xhtml#note3">3</a></sup>.</p>
<p>A link to <a id="next" href="chapter2.xhtml">the next chapter</a>, which is no note.</p>
<p><img id="plate" src="../images/plate.png" alt="A plate"/></p>
<p>{filler}</p>
<aside epub:type="footnote" id="fn1"><p>The first note, in <i>italics</i>.</p></aside>''')
chapter2 = page('More', f'<h1>More</h1>\n<p>{filler}</p>')
notes = page('Endnotes', '''<h1>Endnotes</h1>
<p><a id="note2" href="chapter1.xhtml#ref2">[2]</a> The second note, from another page.</p>
<p id="note3">3. The third note.</p>''')

with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip')
    z.writestr('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" '
               'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
               '<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
               '</rootfiles></container>')
    z.writestr('OEBPS/content.opf', '<?xml version="1.0" encoding="utf-8"?>'
               '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
               '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:uuid:7d0a3c6e-1d1e-4b8e-9b54-000000000025</dc:identifier>'
               '<dc:title>The Notes</dc:title><dc:creator>Ann Example</dc:creator><dc:language>en</dc:language>'
               '<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata><manifest>'
               '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
               '<item id="c1" href="text/chapter1.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="c2" href="text/chapter2.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="n" href="text/notes.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="plate" href="images/plate.png" media-type="image/png"/>'
               '</manifest><spine><itemref idref="c1"/><itemref idref="c2"/><itemref idref="n"/></spine></package>')
    z.writestr('OEBPS/nav.xhtml', '<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" '
               'xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body>'
               '<nav epub:type="toc"><ol><li><a href="text/chapter1.xhtml">Notes</a></li>'
               '<li><a href="text/chapter2.xhtml">More</a></li><li><a href="text/notes.xhtml">Endnotes</a></li>'
               '</ol></nav></body></html>')
    z.writestr('OEBPS/text/chapter1.xhtml', chapter1)
    z.writestr('OEBPS/text/chapter2.xhtml', chapter2)
    z.writestr('OEBPS/text/notes.xhtml', notes)
    z.writestr('OEBPS/images/plate.png', png(400, 200))
