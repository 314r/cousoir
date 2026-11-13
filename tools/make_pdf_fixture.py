#!/usr/bin/env python3
# Writes tests/data/reader/voyage.pdf, a PDF for the PDF reader's tests:
# five pages of made-up text in a standard font, the words of the EPUB
# fixture's, and an outline with a chapter's section nested under it.
#
#   python3 tools/make_pdf_fixture.py tests/data/reader/voyage.pdf

import sys

WORDS = ('river stone lantern harbour quiet north letter window garden morning valley '
         'compass winter story bridge thunder orchard candle meadow voyage').split()
# Each page's heading; the outline's entries say which page they go to.
PAGES = ['The Harbour', 'Arrival', 'The Crossing', 'Storm', 'Landfall']
OUTLINE = [('The Harbour', 0, [('Arrival', 1)]), ('The Crossing', 2, [('Storm', 3)]), ('Landfall', 4, [])]


def lines(page):
    out = []
    for n in range(12):
        words = [WORDS[(page * 7 + n * 3 + i * 5) % len(WORDS)] for i in range(9)]
        out.append(' '.join(words))
    if page == 3:
        out.append('A lighthouse at last.')
    return out


def content(page):
    text = ['BT', '/F1 22 Tf', '72 740 Td', f'({PAGES[page]}) Tj', '/F1 12 Tf', '0 -40 Td']
    for line in lines(page):
        text += [f'({line}) Tj', '0 -18 Td']
    text.append('ET')
    return '\n'.join(text).encode('latin-1')


objects = {}
# 1 catalog, 2 pages, 3 font, 4 outlines; then pages and their contents,
# then the outline's items.
page_ids = [5 + 2 * i for i in range(len(PAGES))]
item_id = 5 + 2 * len(PAGES)
items = []
for title, page, children in OUTLINE:
    entry = {'id': item_id, 'title': title, 'page': page, 'children': []}
    item_id += 1
    for child_title, child_page in children:
        entry['children'].append({'id': item_id, 'title': child_title, 'page': child_page, 'children': []})
        item_id += 1
    items.append(entry)

objects[1] = b'<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R /PageMode /UseOutlines >>'
objects[2] = ('<< /Type /Pages /Kids [%s] /Count %d >>' % (
    ' '.join(f'{p} 0 R' for p in page_ids), len(PAGES))).encode()
objects[3] = b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>'
for i, page_id in enumerate(page_ids):
    objects[page_id] = (f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] '
                        f'/Resources << /Font << /F1 3 0 R >> >> /Contents {page_id + 1} 0 R >>').encode()
    stream = content(i)
    objects[page_id + 1] = b'<< /Length %d >>\nstream\n' % len(stream) + stream + b'\nendstream'


def outline(entries, parent):
    for k, entry in enumerate(entries):
        fields = [f'/Title ({entry["title"]})', f'/Parent {parent} 0 R',
                  f'/Dest [{page_ids[entry["page"]]} 0 R /XYZ 0 792 0]']
        if k > 0:
            fields.append(f'/Prev {entries[k - 1]["id"]} 0 R')
        if k + 1 < len(entries):
            fields.append(f'/Next {entries[k + 1]["id"]} 0 R')
        if entry['children']:
            children = entry['children']
            fields += [f'/First {children[0]["id"]} 0 R', f'/Last {children[-1]["id"]} 0 R',
                       f'/Count {len(children)}']
            outline(children, entry['id'])
        objects[entry['id']] = ('<< ' + ' '.join(fields) + ' >>').encode()


outline(items, 4)
count = sum(1 + len(entry['children']) for entry in items)
objects[4] = f'<< /Type /Outlines /First {items[0]["id"]} 0 R /Last {items[-1]["id"]} 0 R /Count {count} >>'.encode()

out = bytearray(b'%PDF-1.4\n')
offsets = {}
for number in sorted(objects):
    offsets[number] = len(out)
    out += b'%d 0 obj\n' % number + objects[number] + b'\nendobj\n'
xref = len(out)
size = max(objects) + 1
out += b'xref\n0 %d\n0000000000 65535 f \n' % size
for number in range(1, size):
    out += b'%010d 00000 n \n' % offsets[number]
out += b'trailer\n<< /Size %d /Root 1 0 R /Info << /Title (The Voyage) >> >>\nstartxref\n%d\n%%%%EOF\n' % (size, xref)
with open(sys.argv[1], 'wb') as f:
    f.write(out)
