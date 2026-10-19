#!/usr/bin/env python3
# Writes a one-page PDF with a line of text, and optionally an info
# dictionary and an XMP packet:
#
#   make_pdf.py out.pdf "Text" [info.json] [packet.xml]

import json
import sys

out, line = sys.argv[1:3]
info = json.load(open(sys.argv[3], encoding='utf-8')) if len(sys.argv) > 3 else {}
xmp = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else None


def pdf_string(text):
    # ASCII as a literal string, anything else in UTF-16 with a byte order
    # mark, as PDF text strings beyond PDFDocEncoding are.
    if text.isascii():
        return b'(' + text.replace('\\', '\\\\').replace('(', '\\(').replace(')', '\\)').encode('ascii') + b')'
    return b'<' + ('\ufeff' + text).encode('utf-16-be').hex().upper().encode('ascii') + b'>'


stream = f'BT /F1 18 Tf 72 720 Td ({line}) Tj ET'.encode('latin-1')
catalog = b'<< /Type /Catalog /Pages 2 0 R' + (b' /Metadata 6 0 R' if xmp else b'') + b' >>'
objects = [
    catalog,
    b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R'
    b' /Resources << /Font << /F1 5 0 R >> >> >>',
    b'<< /Length %d >>\nstream\n' % len(stream) + stream + b'\nendstream',
    b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
]
if xmp:
    objects.append(b'<< /Type /Metadata /Subtype /XML /Length %d >>\nstream\n' % len(xmp) + xmp + b'\nendstream')
if info:
    objects.append(b'<< ' + b' '.join(b'/%s %s' % (key.encode('ascii'), pdf_string(value)) for key, value in info.items()) + b' >>')
pdf = bytearray(b'%PDF-1.4\n')
offsets = []
for number, body in enumerate(objects, 1):
    offsets.append(len(pdf))
    pdf += b'%d 0 obj\n' % number + body + b'\nendobj\n'
xref = len(pdf)
pdf += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1)
for offset in offsets:
    pdf += b'%010d 00000 n \n' % offset
trailer = b'/Size %d /Root 1 0 R' % (len(objects) + 1)
if info:
    trailer += b' /Info %d 0 R' % len(objects)
pdf += b'trailer\n<< ' + trailer + b' >>\nstartxref\n%d\n%%%%EOF\n' % xref
with open(out, 'wb') as f:
    f.write(pdf)
