# Applies the changes of a JSON file the way Calibre writes them, for the
# golden test that compares Cousoir's writes with Calibre's:
#
#   calibre-debug -e tools/oracle/apply_edits.py -- <library> <changes.json>
#
# The file is cousoir-cli apply's. Edits, [{"book": 1, "set": {"title": ...}}],
# go field by field through Cache.set_field() or set_cover(), in the order
# Cache.set_metadata() sets them. Then, in cousoir-cli's order: formats copied
# from one book to another as Merge only formats copies them, formats to the
# trash with remove_formats(), books with remove_books(), restorations with
# move_book_from_trash() and move_format_from_trash(), books added as
# calibredb add adds them, formats with add_format(). The metadata.opf files
# of the books changed are then written and the books left dirtied, as
# Cousoir leaves them for Calibre to write again.

import json
import os
import sys

from calibre.ebooks.metadata.meta import get_metadata
from calibre.library import db as open_library
from calibre.utils.date import UNDEFINED_DATE
from calibre.utils.localization import _

library, edits_path = sys.argv[-2:]
base = os.path.dirname(os.path.abspath(edits_path))
with open(edits_path, encoding='utf-8') as f:
    changes = json.load(f)

cache = open_library(library).new_api
# The books dirtied before, by an earlier run, Calibre would write again only
# when it next opens the library, as it would Cousoir's.
start = cache.dirtied_sequence
ORDER = ('title', 'authors', 'coverFile', 'removeCover', 'rating', 'seriesIndex', 'authorSort', 'publisher', 'series',
         'tags', 'comments', 'languages', 'pubdate', 'sort', 'identifiers')
FIELDS = {'title': 'title', 'authors': 'authors', 'rating': 'rating', 'seriesIndex': 'series_index',
          'authorSort': 'author_sort', 'publisher': 'publisher', 'series': 'series', 'tags': 'tags',
          'comments': 'comments', 'languages': 'languages', 'pubdate': 'pubdate', 'sort': 'sort',
          'identifiers': 'identifiers'}
OPERATIONS = ('copyFormats', 'removeFormats', 'trash', 'restore', 'add', 'addFormat')


def edit(change):
    book = change['book']
    values = change['set']
    unknown = set(values) - set(ORDER)
    if unknown:
        raise SystemExit('Unknown fields: %s' % ', '.join(sorted(unknown)))
    for key in ORDER:
        if key not in values:
            continue
        value = values[key]
        if key == 'coverFile':
            with open(os.path.join(base, value), 'rb') as f:
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


def add(paths):
    # book() of Calibre's src/calibre/db/cli/cmd_add.py, Copyright (C) Kovid
    # Goyal, GPL v3: the first file's metadata, every file a format.
    paths = [os.path.join(base, p) for p in (paths if isinstance(paths, list) else [paths])]
    fmt = os.path.splitext(paths[0])[1]
    fmt = (fmt[1:] if fmt else None) or 'unknown'
    with open(paths[0], 'rb') as stream:
        mi = get_metadata(stream, stream_type=fmt, use_libprs_metadata=True)
    if not mi.title:
        mi.title = os.path.splitext(os.path.basename(paths[0]))[0]
    if not mi.authors:
        mi.authors = [_('Unknown')]
    format_map = {os.path.splitext(p)[1][1:]: p for p in paths}
    cache.add_books([(mi, format_map)], add_duplicates=True, run_hooks=False)


for change in changes:
    if not any(key in change for key in OPERATIONS):
        edit(change)
for key in OPERATIONS:
    for change in changes:
        if key not in change:
            continue
        value = change[key]
        if key == 'copyFormats':
            # merge_books(merge_only_formats=True): add_formats(…, replace=False).
            for fmt in cache.formats(value['from']):
                path = cache.format_abspath(value['from'], fmt)
                if path:
                    cache.add_format(value['to'], fmt, path, replace=False, run_hooks=False)
        elif key == 'removeFormats':
            cache.remove_formats({value['book']: value['formats']})
        elif key == 'trash':
            cache.remove_books(value)
        elif key == 'restore':
            if isinstance(value, dict):
                cache.move_format_from_trash(value['book'], value['format'])
            else:
                cache.move_book_from_trash(value)
        elif key == 'add':
            add(value)
        elif key == 'addFormat':
            path = os.path.join(base, value['file'])
            cache.add_format(value['book'], os.path.splitext(path)[1][1:], path, replace=True, run_hooks=False)
changed = [book_id for book_id, sequence in cache.dirtied_cache.items() if sequence >= start]
cache.dump_metadata(book_ids=changed, remove_from_dirtied=False)
cache.close()
