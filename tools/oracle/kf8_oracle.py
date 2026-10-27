# Writes tests/data/kf8/oracle.json: what Calibre's KF8 writer makes of
# fixed inputs, for kf8_test.cpp to make the same bytes: PalmDoc compression,
# the text records of a text whose characters cross their ends, the
# skeleton, chunk, guide and NCX indices, the last past 64 KB, and the
# trailing byte sequences of tables of contents over text records. The
# large ones, which the test makes by the same rules, by their SHA-256.
#
#   CALIBRE_OVERRIDE_LANG=en calibre-debug -e tools/oracle/kf8_oracle.py -- tests/data/kf8/oracle.json

import base64
import hashlib
import json
import random
import sys
from collections import namedtuple
from io import BytesIO
from struct import pack

from calibre.ebooks.compression.palmdoc import compress_doc
from calibre.ebooks.mobi.utils import create_text_record, encint, encode_trailing_data
from calibre.ebooks.mobi.writer8.index import ChunkIndex, GuideIndex, NCXIndex, SkelIndex
from calibre.ebooks.mobi.writer8.tbs import apply_trailing_byte_sequences

rng = random.Random(8)
words = 'lantern winter candle river north valley thunder été déjà naïve 日本語 ünïcödé — « » <p class="x">'.split()


def prose(size):
    out = []
    length = 0
    while length < size:
        word = rng.choice(words)
        out.append(word)
        length += len(word.encode('utf-8')) + 1
    return ' '.join(out).encode('utf-8')[:size].decode('utf-8', 'ignore').encode('utf-8')


palmdoc = []
for data in (b'', b'a', prose(4096), prose(1000), bytes(rng.randrange(256) for _ in range(700)),
             b'abc' * 600, b' Aa' * 200 + b'\x00\x01\x02\x08\x09\x7f\x80\xff' * 30,
             (b'<p aid="X">' + prose(60) + b'</p>\n') * 40):
    palmdoc.append({'input': base64.b64encode(data).decode('ascii'), 'output': base64.b64encode(compress_doc(data)).decode('ascii')})

# create_text_records() of KF8Writer, over a text cut inside its characters.
text = ''.join(f'{i} lantern été 日本語 ' for i in range(4000)).encode('utf-8')
stream = BytesIO(text)
text_records = []
lengths = []
while stream.tell() < len(text):
    data, overlap = create_text_record(stream)
    lengths.append(len(data))
    text_records.append(compress_doc(data) + overlap + pack(b'>B', len(overlap)))

Skel = namedtuple('Skel', 'file_number name chunk_count start_pos length')
Chunk = namedtuple('Chunk', 'insert_pos selector file_number sequence_number start_pos length')
GuideRef = namedtuple('GuideRef', 'title type pos_fid')

skeletons = []
chunks = []
start = 0
sequence = 0
for file_number in range(12):
    count = rng.randrange(0, 6)
    length = rng.randrange(200, 900)
    skeletons.append(Skel(file_number, f'SKEL{file_number:010}', count, start, length))
    within = 0
    for _ in range(count):
        size = rng.randrange(100, 8192)
        aid = format(file_number * 1000000 + rng.randrange(1000), 'X')
        selector = "{}-//*[@aid='{}']".format(rng.choice('PS'), aid)
        chunks.append(Chunk(start + length // 2 + within, selector, file_number, sequence, within, size))
        within += size
        sequence += 1
    start += length + within
guide = [GuideRef('Table of Contents', 'toc', (3, 0)), GuideRef('Start', 'text', (1, 120)), GuideRef('', 'cover', (0, 0))]
guide.sort(key=lambda x: x.type)


def toc(count, width, label_size):
    # A table of contents of `count` entries, by depth, then by place.
    entries = []
    parents = []
    offset = 0
    for i in range(count):
        depth = 0 if not entries or rng.random() < 0.3 else min(rng.randrange(0, 3), entries[-1]['depth'] + 1)
        entries.append({'depth': depth, 'offset': offset, 'label': ' '.join(rng.choice(words) for _ in range(label_size))})
        offset += rng.randrange(1, width)
    # Parents: the nearest entry before at the depth above.
    for i, entry in enumerate(entries):
        if entry['depth']:
            for j in range(i - 1, -1, -1):
                if entries[j]['depth'] == entry['depth'] - 1:
                    entry['parent_at'] = j
                    break
            else:
                entry['depth'] = 0
    order = sorted(range(count), key=lambda i: (entries[i]['depth'], entries[i]['offset']))
    index_of = {old: new for new, old in enumerate(order)}
    table = []
    for old in order:
        entry = entries[old]
        item = {'index': index_of[old], 'label': entry['label'], 'offset': entry['offset'], 'depth': entry['depth'],
                'pos_fid': (rng.randrange(0, 50), rng.randrange(0, 8000))}
        if 'parent_at' in entry:
            item['parent'] = index_of[entry['parent_at']]
        children = [index_of[i] for i, e in enumerate(entries) if e.get('parent_at') == old]
        if children:
            item['first_child'] = min(children)
            item['last_child'] = max(children)
        table.append(item)
    for item in table:
        enders = [e['offset'] for e in table if e['depth'] <= item['depth'] and e['offset'] > item['offset']]
        item['length'] = (min(enders) if enders else offset) - item['offset']
    return table, offset


def records_hex(records):
    return [base64.b64encode(r).decode('ascii') for r in records]


def digest(records):
    return hashlib.sha256(b''.join(records)).hexdigest()


ncx_small, _ = toc(40, 3000, 2)

# 825 chapters of three parts each, in the index's order: the chapters,
# then the parts.
ncx_large = []
for t in range(825):
    ncx_large.append({'index': t, 'label': f'Chapter {t}', 'offset': t * 1000, 'length': 1000, 'depth': 0,
                      'pos_fid': (t, 0), 'first_child': 825 + 3 * t, 'last_child': 825 + 3 * t + 2})
for t in range(825):
    for k in range(3):
        ncx_large.append({'index': 825 + 3 * t + k, 'label': f'Part {k} of chapter {t}, by the lantern and the river',
                          'offset': t * 1000 + 100 + 300 * k, 'length': 300, 'depth': 1, 'pos_fid': (t, 10 * k), 'parent': t})

tbs = []
for count, width in ((12, 9000), (60, 2500), (200, 700), (30, 20000)):
    table, size = toc(count, width, 1)
    record_lengths = [4096] * (size // 4096) + ([size % 4096] if size % 4096 else [])
    records = [b''] * (len(record_lengths) + 1)
    apply_trailing_byte_sequences(table, records, record_lengths)
    tbs.append({'ncx': table, 'record_lengths': record_lengths, 'trailing': records_hex(records[1:])})


def jsonable(table):
    keys = ('index', 'offset', 'length', 'depth', 'parent', 'first_child', 'last_child')
    return [[e.get(k, -1) for k in keys] + list(e['pos_fid']) + [e['label']] for e in table]


oracle = {
    'palmdoc': palmdoc,
    'encint': [{'value': v, 'forward': encint(v).hex(), 'backward': encint(v, forward=False).hex(),
                'trailing': encode_trailing_data(b'\x01' * (v % 300)).hex()} for v in (0, 1, 127, 128, 300, 16383, 16384, 2**21, 2**28 + 5)],
    'text_records': digest(text_records),
    'text_lengths': lengths,
    'skeletons': [s._asdict() for s in skeletons],
    'skeleton_index': records_hex(SkelIndex(skeletons)()),
    'chunks': [c._asdict() for c in chunks],
    'chunk_index': records_hex(ChunkIndex(chunks)()),
    'guide': [{'title': g.title, 'type': g.type, 'pos_fid': list(g.pos_fid)} for g in guide],
    'guide_index': records_hex(GuideIndex(guide)()),
    'ncx_small': jsonable(ncx_small),
    'ncx_small_index': records_hex(NCXIndex(ncx_small)()),
    'ncx_large_index': digest(NCXIndex(ncx_large)()),
    'ncx_large_records': len(NCXIndex(ncx_large)()),
    'tbs': [dict(t, ncx=jsonable(t['ncx'])) for t in tbs],
}
with open(sys.argv[-1], 'w') as f:
    json.dump(oracle, f, ensure_ascii=False, indent=0, sort_keys=True)
    f.write('\n')
