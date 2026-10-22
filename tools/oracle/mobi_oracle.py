# Writes tests/data/mobi: MOBI and AZW3 books, the metadata cases
# mobi_metadata_test.cpp writes into them, and what Calibre's own MOBI
# metadata writer (MetadataUpdater of ebooks/metadata/mobi.py, as a send to
# a Kindle runs it) makes of each, its random ASIN drawn as a fixed one:
#
#   calibre-debug -e tools/oracle/mobi_oracle.py -- tests/data
#
# The books are the fixtures' Calibre conversions, and two Calibre's
# converter never writes: one without an EXTH header whose MOBI header is
# 0xE4 bytes long, and an AZW3 without an ASIN. Of the first, Calibre loses
# the text: adding the EXTH header grows the file, but its slice of the file
# keeps the first length, so the second rewrite reads the last record as
# empty. Cousoir keeps it; the test compares the first record only.

import io
import json
import os
import shutil
import struct
import sys
import uuid

from calibre.ebooks.metadata.book.base import Metadata
from calibre.utils.date import parse_date

data = sys.argv[-1]
out = os.path.join(data, 'mobi')
os.makedirs(os.path.join(out, 'calibre'), exist_ok=True)

shutil.copyfile(os.path.join(data, 'import', 'Vicomte.azw3'), os.path.join(out, 'vicomte.azw3'))
shutil.copyfile(os.path.join(data, 'import', 'Vicomte.mobi'), os.path.join(out, 'vicomte.mobi'))
black = os.path.join(data, 'library', 'Glen Cook', 'The Black Company (2)', 'The Black Company - Glen Cook.azw3')
shutil.copyfile(black, os.path.join(out, 'black.azw3'))


def records(raw):
    count = struct.unpack('>H', raw[76:78])[0]
    offsets = [struct.unpack('>I', raw[78 + 8 * i:82 + 8 * i])[0] for i in range(count)]
    return [raw[offsets[i]:offsets[i + 1] if i + 1 < count else len(raw)] for i in range(count)], offsets


def rebuild(raw, first):
    recs, offsets = records(raw)
    head = bytearray(raw[:offsets[0]])
    body = first
    position = offsets[0] + len(first)
    for i in range(1, len(recs)):
        head[78 + 8 * i:82 + 8 * i] = struct.pack('>I', position)
        body += recs[i]
        position += len(recs[i])
    return bytes(head) + body


# The Black Company without its ASIN: its EXTH without 113.
raw = open(black, 'rb').read()
recs, offsets = records(raw)
r0 = recs[0]
header = struct.unpack('>I', r0[20:24])[0]
start = 16 + header
count = struct.unpack('>I', r0[start + 8:start + 12])[0]
pos = start + 12
kept = []
for i in range(count):
    rid, size = struct.unpack('>II', r0[pos:pos + 8])
    if rid != 113:
        kept.append(r0[pos:pos + size])
    pos += size
body = b''.join(kept)
exth = b'EXTH' + struct.pack('>II', len(body) + 12, len(kept)) + body + b'\0' * (4 - len(body) % 4)
old_length = struct.unpack('>I', r0[start + 4:start + 8])[0]
old_end = start + old_length + (4 - old_length % 4 if old_length % 4 else 4)
title_offset, title_length = struct.unpack('>II', r0[0x54:0x5C])
first = bytearray(r0[:start] + exth + r0[title_offset:title_offset + title_length] + b'\0' * 4096)
first[0x54:0x58] = struct.pack('>I', start + len(exth))
open(os.path.join(out, 'no-asin.azw3'), 'wb').write(rebuild(raw, bytes(first)))

# A bare MOBI: a PalmDOC header, a MOBI header of 0xE4 bytes, cp1252, no
# EXTH, its title after the header, and a record of text.
text = b'<html><body><p>' + b'Plain text, again and again. ' * 200 + b'</p></body></html>'
mobi = bytearray(16 + 0xE4)
mobi[0:2] = struct.pack('>H', 1)
mobi[4:8] = struct.pack('>I', len(text))
mobi[8:10] = struct.pack('>H', 1)
mobi[10:12] = struct.pack('>H', 4096)
mobi[16:20] = b'MOBI'
mobi[20:24] = struct.pack('>I', 0xE4)
mobi[24:28] = struct.pack('>I', 2)
mobi[28:32] = struct.pack('>I', 1252)
mobi[0x68:0x6C] = struct.pack('>I', 6)
mobi[0x6C:0x70] = struct.pack('>I', 0xFFFFFFFF)
mobi[0x80:0x84] = struct.pack('>I', 0)
title = 'Caf\xe9 bare'.encode('cp1252')
mobi[0x54:0x58] = struct.pack('>I', len(mobi))
mobi[0x58:0x5C] = struct.pack('>I', len(title))
mobi += title + b'\0' * 2
first = bytes(mobi)
name = b'Bare_title'.ljust(32, b'\0')
pdb = bytearray(name + b'\0' * (78 - 32))
pdb[60:68] = b'BOOKMOBI'
pdb[76:78] = struct.pack('>H', 2)
offset = 78 + 8 * 2 + 2
pdb += struct.pack('>IBBBB', offset, 0, 0, 0, 0) + struct.pack('>IBBBB', offset + len(first), 0, 0, 0, 2) + b'\0\0'
open(os.path.join(out, 'bare.mobi'), 'wb').write(bytes(pdb) + first + text)

cover = os.path.join(data, 'library', 'Glen Cook', 'The Black Company (2)', 'cover.jpg')
cases = {
    'full': {
        'title': 'L’Écume des jours', 'authors': ['Boris Vian', 'Ren\xe9 Łojek'],
        'author_sort': 'Vian, Boris & Łojek, Ren\xe9', 'publisher': 'Gallimard',
        'comments': '<div><p>Un roman d’amour fou.</p></div><div class="user_annotations">old</div>',
        'isbn': '9782253140870', 'tags': ['Roman', 'Classique ★'], 'pubdate': '1947-03-20T00:00:00+00:00',
        'uuid': '94357bfe-c867-4007-956a-4c125dd8ad7e', 'languages': ['fra', 'eng'], 'options': {},
    },
    'bare': {
        'title': 'Untitled', 'authors': [], 'author_sort': '', 'publisher': '', 'comments': '', 'isbn': '',
        'tags': [], 'pubdate': '0101-01-01T00:00:00+00:00', 'uuid': '', 'languages': [], 'options': {},
    },
    'options': {
        'title': 'Shadows Linger', 'authors': ['Glen Cook'], 'author_sort': 'Cook, Glen', 'publisher': 'Tor',
        'comments': '', 'isbn': '', 'tags': ['Fantasy', 'Dark'], 'pubdate': '1984-05-02T12:00:00+00:00',
        'uuid': 'f30e1f72-acfb-4c1c-98fd-71b2a3ffc29e', 'languages': ['eng'],
        'options': {'prefer_author_sort': True, 'personal_doc': 'Dark', 'share_not_sync': False},
    },
    'cover': {
        'title': 'The Black Company', 'authors': ['Glen Cook'], 'author_sort': 'Cook, Glen', 'publisher': '',
        'comments': '', 'isbn': '', 'tags': [], 'pubdate': '1984-05-02T00:00:00+00:00', 'uuid': '',
        'languages': ['eng'], 'options': {}, 'cover': 'cover.jpg',
    },
}
shutil.copyfile(cover, os.path.join(out, 'cover.jpg'))
with open(os.path.join(out, 'cases.json'), 'w', encoding='utf-8') as f:
    json.dump(cases, f, ensure_ascii=False, indent=1, sort_keys=True)

import calibre.ebooks.conversion.config as config
from calibre.ebooks.metadata import mobi as mobi_metadata

fixed = uuid.UUID('00000000-0000-4000-8000-000000000001')
uuid.uuid4 = lambda: fixed
for book in sorted(os.listdir(out)):
    if not book.endswith(('.mobi', '.azw3')):
        continue
    for case, values in cases.items():
        config.load_defaults = lambda name, values=values: dict(values['options'])
        mi = Metadata(values['title'], values['authors'] or None)
        if not values['authors']:
            mi.authors = []
        mi.author_sort = values['author_sort'] or None
        mi.publisher = values['publisher'] or None
        mi.comments = values['comments'] or None
        if values['isbn']:
            mi.set_identifier('isbn', values['isbn'])
        mi.tags = values['tags']
        mi.pubdate = parse_date(values['pubdate'], assume_utc=True)
        mi.uuid = values['uuid'] or None
        mi.languages = values['languages']
        if values.get('cover'):
            mi.cover = os.path.join(out, values['cover'])
        stream = io.BytesIO(open(os.path.join(out, book), 'rb').read())
        try:
            mobi_metadata.MetadataUpdater(stream).update(mi)
            result = stream.getvalue()
        except Exception as e:
            result = ('error: %s' % e).encode('utf-8')
        stem, ext = os.path.splitext(book)
        open(os.path.join(out, 'calibre', '%s-%s%s' % (stem, case, ext)), 'wb').write(result)

# What Calibre's Kindle driver makes of each: the cover thumbnail's name,
# and the APNX of the fast method, its random guid drawn as the fixed one.
from calibre.devices.kindle.apnx import APNXBuilder
from calibre.devices.kindle.driver import thumbnail_filename

names = {}
for written in sorted(os.listdir(os.path.join(out, 'calibre'))):
    path = os.path.join(out, 'calibre', written)
    if not written.endswith(('.mobi', '.azw3')) or open(path, 'rb').read(6) == b'error:':
        continue
    with open(path, 'rb') as f:
        names[written] = thumbnail_filename(f)
    try:
        APNXBuilder().write_apnx(path, path + '.apnx', method='fast')
    except Exception as e:
        names[written + '.apnx'] = 'error: %s' % e
with open(os.path.join(out, 'calibre', 'thumbnails.json'), 'w') as f:
    json.dump(names, f, indent=1, sort_keys=True)
