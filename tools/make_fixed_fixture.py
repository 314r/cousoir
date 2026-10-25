#!/usr/bin/env python3
# Writes tests/data/reader/fixed.epub, a picture book of fixed pages for the
# reader: a cover alone, a spread its pages place, a spread of a page of a
# drawing and a page of a picture, neither with a viewport, the reader
# places, a page alone at the centre, and
# a page of credits that flows as text does.
#
#   python3 tools/make_fixed_fixture.py tests/data/reader/fixed.epub

import struct
import sys
import zipfile
import zlib


def png(width, height, colour):
    rows = b''
    for y in range(height):
        shade = tuple(max(0, c - (y * 60 // height)) for c in colour)
        rows += b'\x00' + bytes(shade) * width
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def page(title, body, head=''):
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en">'
            f'<head><title>{title}</title>{head}</head>\n<body>\n{body}\n</body></html>\n')


viewport = '<meta name="viewport" content="width=600, height=800"/>'


def fixed(number, colour, words):
    style = (f'<style>body {{ margin: 0; width: 600px; height: 800px; background: {colour}; position: relative; }}'
             ' h1 { position: absolute; left: 40px; top: 60px; margin: 0; font-size: 64px; color: #222; }'
             ' p { position: absolute; left: 40px; bottom: 60px; margin: 0; font-size: 28px; color: #222; }</style>')
    return page(f'Page {number}', f'<h1 id="page{number}">Page {number}</h1>\n<p>{words}</p>', viewport + style)


svg = ('<svg xmlns="http://www.w3.org/2000/svg" version="1.1" viewBox="0 0 600 800" width="100%" height="100%">'
       '<rect width="600" height="800" fill="#b8d8f0"/>'
       '<text x="40" y="120" font-size="64" fill="#222">Page 4</text>'
       '<text x="40" y="740" font-size="28" fill="#222">The sea, drawn.</text></svg>')
pages = {
    'cover': fixed(1, '#f0d8b8', 'The Picture Book'),
    'p2': fixed(2, '#d8f0b8', 'A garden on the left.'),
    'p3': fixed(3, '#f0b8d8', 'A house on the right.'),
    'p4': page('Page 4', svg, '<style>body { margin: 0; }</style>'),
    'p5': page('Page 5', '<div><img src="../images/sky.png" alt="The sky"/></div>'),
    'p6': fixed(6, '#e0e0e0', 'A page alone.'),
    'credits': page('Credits', '<h1>Credits</h1>\n<p>Words and pictures by Ann Example, set as text that flows.</p>'),
}

with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip')
    z.writestr('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" '
               'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
               '<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
               '</rootfiles></container>')
    z.writestr('OEBPS/content.opf', '<?xml version="1.0" encoding="utf-8"?>'
               '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
               '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:uuid:7d0a3c6e-1d1e-4b8e-9b54-000000000026</dc:identifier>'
               '<dc:title>The Picture Book</dc:title><dc:creator>Ann Example</dc:creator><dc:language>en</dc:language>'
               '<meta property="rendition:layout">pre-paginated</meta><meta property="rendition:spread">auto</meta>'
               '<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata><manifest>'
               '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
               '<item id="cover" href="text/cover.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="p2" href="text/p2.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="p3" href="text/p3.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="p4" href="text/p4.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="p5" href="text/p5.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="p6" href="text/p6.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="credits" href="text/credits.xhtml" media-type="application/xhtml+xml"/>'
               '<item id="sky" href="images/sky.png" media-type="image/png"/>'
               '</manifest><spine>'
               '<itemref idref="cover"/>'
               '<itemref idref="p2" properties="page-spread-left"/>'
               '<itemref idref="p3" properties="page-spread-right"/>'
               '<itemref idref="p4"/>'
               '<itemref idref="p5"/>'
               '<itemref idref="p6" properties="rendition:page-spread-center"/>'
               '<itemref idref="credits" properties="rendition:layout-reflowable"/>'
               '</spine></package>')
    z.writestr('OEBPS/nav.xhtml', page('Contents', '<nav epub:type="toc"><ol>'
               '<li><a href="text/cover.xhtml">Cover</a></li>'
               '<li><a href="text/p3.xhtml">The house</a></li>'
               '<li><a href="text/p4.xhtml">The sea</a></li>'
               '<li><a href="text/credits.xhtml">Credits</a></li></ol></nav>'))
    for name, text in pages.items():
        z.writestr(f'OEBPS/text/{name}.xhtml', text)
    z.writestr('OEBPS/images/sky.png', png(600, 800, (150, 190, 240)))
