#!/usr/bin/env python3
# Writes a one-page PDF with a line of text: make_pdf.py out.pdf "Text"

import sys

out, line = sys.argv[1:3]
stream = f'BT /F1 18 Tf 72 720 Td ({line}) Tj ET'.encode('latin-1')
objects = [
    b'<< /Type /Catalog /Pages 2 0 R >>',
    b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R'
    b' /Resources << /Font << /F1 5 0 R >> >> >>',
    b'<< /Length %d >>\nstream\n' % len(stream) + stream + b'\nendstream',
    b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
]
pdf = bytearray(b'%PDF-1.4\n')
offsets = []
for number, body in enumerate(objects, 1):
    offsets.append(len(pdf))
    pdf += b'%d 0 obj\n' % number + body + b'\nendobj\n'
xref = len(pdf)
pdf += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1)
for offset in offsets:
    pdf += b'%010d 00000 n \n' % offset
pdf += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objects) + 1, xref)
with open(out, 'wb') as f:
    f.write(pdf)
