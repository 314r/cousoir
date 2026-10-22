# Writes tests/data/epubmeta/expected: what Calibre's metadata writer makes of
# each package of tests/data/epubmeta/packages with each set of metadata of
# tests/data/epubmeta/metadata.json, the package's folder being OEBPS, as
# ebooks/metadata/epub.py's set_metadata() calls it for a book it sends:
# <package>-<set>.opf, and the cover's address in covers.json.
#
#   CALIBRE_OVERRIDE_LANG=en calibre-debug -e tools/oracle/epub_meta_oracle.py -- tests/data/epubmeta

import copy
import json
import os
import sys

from calibre.ebooks.metadata.book.base import Metadata
from calibre.ebooks.metadata.book.json_codec import decode_is_multiple
from calibre.ebooks.metadata.opf import set_metadata
from calibre.utils.date import UNDEFINED_DATE, parse_date

folder = sys.argv[-1]
with open(os.path.join(folder, 'metadata.json'), encoding='utf-8') as f:
    sets = json.load(f)
out = os.path.join(folder, 'expected')
os.makedirs(out, exist_ok=True)
covers = {}


def metadata(m):
    # db.get_metadata()'s: every field set, the custom columns with theirs.
    mi = Metadata(m['title'], m['authors'])
    mi.title_sort = m['title_sort']
    mi.author_sort = m['author_sort']
    mi.languages = m['languages']
    mi.pubdate = parse_date(m['pubdate']) if m['pubdate'] else UNDEFINED_DATE
    mi.comments = m['comments']
    mi.publisher = m['publisher']
    mi.tags = m['tags']
    mi.rating = m['rating']
    mi.series = m['series']
    mi.series_index = m['series_index']
    mi.set_identifiers(dict(m['identifiers']))
    mi.application_id = m['application_id']
    mi.uuid = m['uuid']
    for key, fm in m['user_metadata'].items():
        fm = copy.deepcopy(fm)
        decode_is_multiple(fm)
        mi.set_user_metadata(key, fm)
    return mi


for name in sorted(os.listdir(os.path.join(folder, 'packages'))):
    with open(os.path.join(folder, 'packages', name), 'rb') as f:
        raw = f.read()
    for key, m in sets.items():
        cover = b'\xff\xd8\xff\xe0cover' if m['cover'] else None
        opf, ver, raster = set_metadata(raw, metadata(m), cover_prefix='OEBPS', cover_data=cover)
        base = name.rpartition('.')[0] + '-' + key
        with open(os.path.join(out, base + '.opf'), 'wb') as f:
            f.write(opf)
        covers[base] = raster
with open(os.path.join(out, 'covers.json'), 'w', encoding='utf-8') as f:
    json.dump(covers, f, indent=1, sort_keys=True)
    f.write('\n')
