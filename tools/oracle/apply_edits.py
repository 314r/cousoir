# Applies the edits of a JSON file the way Calibre writes them, for the
# golden test that compares Reliure's writes with Calibre's:
#
#   calibre-debug -e tools/oracle/apply_edits.py -- <library> <edits.json>
#
# The file is reliure-cli apply's: [{"book": 1, "set": {"title": ..., ...}}].
# Each field goes through Cache.set_field() or set_cover(), in the order
# Cache.set_metadata() sets them; metadata.opf files are then written and the
# books left dirtied, as Reliure leaves them for Calibre to write again.

import json
import os
import sys

from calibre.library import db as open_library
from calibre.utils.date import UNDEFINED_DATE

library, edits_path = sys.argv[-2:]
with open(edits_path, encoding='utf-8') as f:
    edits = json.load(f)

cache = open_library(library).new_api
ORDER = ('title', 'authors', 'coverFile', 'removeCover', 'rating', 'seriesIndex', 'authorSort', 'publisher', 'series',
         'tags', 'comments', 'languages', 'pubdate', 'sort', 'identifiers')
FIELDS = {'title': 'title', 'authors': 'authors', 'rating': 'rating', 'seriesIndex': 'series_index',
          'authorSort': 'author_sort', 'publisher': 'publisher', 'series': 'series', 'tags': 'tags',
          'comments': 'comments', 'languages': 'languages', 'pubdate': 'pubdate', 'sort': 'sort',
          'identifiers': 'identifiers'}
for edit in edits:
    book = edit['book']
    changes = edit['set']
    unknown = set(changes) - set(ORDER)
    if unknown:
        raise SystemExit('Unknown fields: %s' % ', '.join(sorted(unknown)))
    for key in ORDER:
        if key not in changes:
            continue
        value = changes[key]
        if key == 'coverFile':
            with open(os.path.join(os.path.dirname(os.path.abspath(edits_path)), value), 'rb') as f:
                cache.set_cover({book: f.read()})
        elif key == 'removeCover':
            if value:
                cache.set_cover({book: None})
        else:
            if key == 'pubdate' and not value.strip():
                # Calibre's editor clears a date to the undefined one.
                value = UNDEFINED_DATE
            elif key == 'authors' and isinstance(value, str):
                value = [a.strip() for a in value.split('&')]
            elif key in ('tags', 'languages') and isinstance(value, str):
                value = [t.strip() for t in value.split(',')]
            elif key == 'identifiers' and isinstance(value, str):
                value = dict(part.strip().partition(':')[0::2] for part in value.split(',') if part.strip())
            cache.set_field(FIELDS[key], {book: value})
cache.dump_metadata(remove_from_dirtied=False)
cache.close()
