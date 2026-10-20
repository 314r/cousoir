# Prints the metadata Calibre reads to add each book file, as calibredb add
# reads it, in the shape `cousoir-cli read` prints Cousoir's:
#
#   calibre-debug -e tools/oracle/read_oracle.py -- <output.json> <file>…
#
# The JSON goes to a file: Calibre's PDF reader prints poppler's warnings.
#
# The empty values Calibre spells "Unknown" or None are printed empty, the
# languages as the library stores them.

import hashlib
import json
import os
import sys

from calibre.ebooks.metadata.meta import get_metadata
from calibre.utils.date import isoformat, is_date_undefined
from calibre.utils.imghdr import identify
from calibre.utils.localization import _, canonicalize_lang


def date(value):
    if value is None or is_date_undefined(value):
        return None
    return isoformat(value, as_utc=True).replace('+00:00', 'Z')


def read(path):
    # book() of Calibre's src/calibre/db/cli/cmd_add.py, Copyright (C) Kovid
    # Goyal, GPL v3.
    fmt = os.path.splitext(path)[1]
    fmt = (fmt[1:] if fmt else None) or 'unknown'
    with open(path, 'rb') as stream:
        mi = get_metadata(stream, stream_type=fmt, use_libprs_metadata=True)
    if not mi.title:
        mi.title = os.path.splitext(os.path.basename(path))[0]
    if not mi.authors:
        mi.authors = [_('Unknown')]
    cover = None
    data = (mi.cover_data or (None, None))[1]
    if data:
        _fmt, width, height = identify(data)
        cover = {'size': len(data), 'sha1': hashlib.sha1(data).hexdigest(), 'width': width, 'height': height}
    languages = [canonicalize_lang(x) for x in (mi.languages or [])]
    return {
        'file': os.path.basename(path),
        'title': mi.title,
        'title_sort': mi.title_sort or '',
        'authors': list(mi.authors),
        'author_sort': '' if mi.is_null('author_sort') else mi.author_sort,
        'series': mi.series or '',
        'series_index': mi.series_index if mi.series else None,
        'tags': list(mi.tags or []),
        'publisher': mi.publisher or '',
        'pubdate': date(mi.pubdate),
        'timestamp': date(mi.timestamp),
        'uuid': mi.uuid or '',
        'comments': mi.comments or '',
        'languages': [x for x in languages if x],
        'identifiers': dict(mi.get_identifiers()),
        'rating': mi.rating,
        'book_producer': mi.book_producer or '',
        'cover': cover,
    }


books = [read(path) for path in sys.argv[2:]]
with open(sys.argv[1], 'w', encoding='utf-8') as f:
    json.dump(books, f, ensure_ascii=False, indent=4)
