#!/usr/bin/env python3
# Writes tests/data/reader/voyage.epub, a book for the reader's tests and
# trials: three chapters of made-up text, long enough for several pages each,
# a table of contents with a chapter's sections nested, a note linked from
# another chapter, an image and an SVG cover page.
#
#   python3 tools/make_reader_fixture.py tests/data/reader/voyage.epub

import base64
import sys
import zipfile

WORDS = ('river stone lantern harbour quiet north letter window garden morning valley '
         'compass winter story bridge thunder orchard candle meadow voyage').split()


def paragraph(chapter, number):
    words = [WORDS[(chapter * 7 + number * 3 + i * 5) % len(WORDS)] for i in range(60)]
    text = ' '.join(words)
    return f'Chapter {chapter}, paragraph {number}: {text[0].upper()}{text[1:]}.'


def chapter(number, title, sections):
    body = [f'<h1 id="top">{title}</h1>']
    for s, section in enumerate(sections):
        body.append(f'<h2 id="s{s + 1}">{section}</h2>')
        for p in range(14):
            text = paragraph(number, s * 14 + p)
            if number == 1 and s == 0 and p == 2:
                text += ' <a id="noteref" href="chapter3.xhtml#note1">See the note.</a>'
            body.append(f'<p>{text}</p>')
        if number == 2 and s == 0:
            body.append('<p><img src="../images/map.png" alt="A map"/></p>')
    if number == 3:
        body.append('<p id="note1">The note: rivers remember every stone.</p>')
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml" lang="en"><head><title>' + title + '</title>'
            '<link rel="stylesheet" href="../style.css" type="text/css"/></head>\n<body>\n'
            + '\n'.join(body) + '\n</body></html>\n')


PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAEklEQVR4nGNgYGD4z8DAwMDAAAAJ/AH/5f8bCwAAAABJRU5ErkJggg==')

chapters = [
    ('chapter1.xhtml', 'The Harbour', ['Arrival', 'The Lantern']),
    ('chapter2.xhtml', 'The Valley', ['The Bridge', 'Thunder']),
    ('chapter3.xhtml', 'The North', ['Winter']),
]
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip')
    z.writestr('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" '
               'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
               '<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
               '</rootfiles></container>')
    items = ''.join(f'<item id="c{i + 1}" href="text/{name}" media-type="application/xhtml+xml"/>'
                    for i, (name, _, _) in enumerate(chapters))
    spine = ''.join(f'<itemref idref="c{i + 1}"/>' for i in range(len(chapters)))
    z.writestr('OEBPS/content.opf', '<?xml version="1.0" encoding="utf-8"?>'
               '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
               '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:uuid:7d0a3c6e-1d1e-4b8e-9b54-000000000019</dc:identifier>'
               '<dc:title>The Voyage</dc:title><dc:creator>Ann Example</dc:creator><dc:language>en</dc:language>'
               '<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata><manifest>'
               '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
               '<item id="cover" href="text/cover.xhtml" media-type="application/xhtml+xml" properties="svg"/>'
               '<item id="css" href="style.css" media-type="text/css"/>'
               '<item id="map" href="images/map.png" media-type="image/png"/>'
               + items + '</manifest><spine><itemref idref="cover"/>' + spine + '</spine></package>')
    toc = ''.join(
        f'<li><a href="text/{name}">{title}</a><ol>'
        + ''.join(f'<li><a href="text/{name}#s{s + 1}">{section}</a></li>' for s, section in enumerate(sections))
        + '</ol></li>' for name, title, sections in chapters)
    z.writestr('OEBPS/nav.xhtml', '<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" '
               'xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body>'
               '<nav epub:type="toc"><ol>' + toc + '</ol></nav></body></html>')
    z.writestr('OEBPS/text/cover.xhtml', '<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml">'
               '<head><title>Cover</title></head><body><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 600 900" '
               'width="100%" height="100%"><rect width="600" height="900" fill="#335"/><text x="300" y="450" '
               'font-size="60" text-anchor="middle" fill="#fff">The Voyage</text></svg></body></html>')
    z.writestr('OEBPS/style.css', 'p { text-indent: 1.5em; margin: 0; line-height: 1.5; color: #222; }\nh1 { color: #800; }\n')
    z.writestr('OEBPS/images/map.png', PNG)
    for name, title, sections in chapters:
        z.writestr('OEBPS/text/' + name, chapter(chapters.index((name, title, sections)) + 1, title, sections))
