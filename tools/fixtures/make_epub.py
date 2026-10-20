#!/usr/bin/env python3
# Writes a minimal valid EPUB 3 for the fixture library:
#
#   make_epub.py out.epub "Title" "Author" lang "Paragraph" ...

import sys
import uuid
import zipfile
from xml.sax.saxutils import escape

out, title, author, language, *paragraphs = sys.argv[1:]

opf = f'''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, 'cousoir-fixture:' + title)}</dc:identifier>
    <dc:title>{escape(title)}</dc:title>
    <dc:creator>{escape(author)}</dc:creator>
    <dc:language>{language}</dc:language>
    <meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="text" href="text.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="text"/>
  </spine>
</package>
'''
nav = f'''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>{escape(title)}</title></head>
<body><nav epub:type="toc"><ol><li><a href="text.xhtml">{escape(title)}</a></li></ol></nav></body>
</html>
'''
body = ''.join(f'<p>{escape(p)}</p>' for p in paragraphs)
text = f'''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>{escape(title)}</title></head>
<body><h1>{escape(title)}</h1>{body}</body>
</html>
'''
container = '''<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
'''

with zipfile.ZipFile(out, 'w') as epub:
    epub.writestr(zipfile.ZipInfo('mimetype', (2026, 1, 1, 0, 0, 0)), 'application/epub+zip', zipfile.ZIP_STORED)
    for name, data in (('META-INF/container.xml', container), ('OEBPS/content.opf', opf),
                       ('OEBPS/nav.xhtml', nav), ('OEBPS/text.xhtml', text)):
        epub.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), data, zipfile.ZIP_DEFLATED)
