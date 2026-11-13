# Has Calibre open a library cousoir-cli made and added a book to
# (tests/golden/run.sh): its metadata.db the same as one Calibre makes,
# but for its own uuid; the book listed; another added by Calibre itself.
# Prints what differs and exits with 1.
#
#   calibre-debug -e tools/oracle/new_library_check.py -- LIBRARY TESTS_DATA

import os
import sys
import tempfile

import apsw

from calibre.ebooks.metadata.meta import get_metadata
from calibre.library import db as open_library

library, data = sys.argv[-2:]
problems = []


def describe(path):
    conn = apsw.Connection(path, flags=apsw.SQLITE_OPEN_READONLY)
    cursor = conn.cursor()
    shape = {
        'schema': list(cursor.execute('SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY rowid')),
        'application_id': next(cursor.execute('PRAGMA application_id'))[0],
        'user_version': next(cursor.execute('PRAGMA user_version'))[0],
        'preferences': list(cursor.execute('SELECT key, val FROM preferences ORDER BY key')),
        'uuid': next(cursor.execute('SELECT uuid FROM library_id'))[0],
    }
    conn.close()
    return shape


ours = describe(os.path.join(library, 'metadata.db'))
with tempfile.TemporaryDirectory() as folder:
    open_library(folder).close()
    theirs = describe(os.path.join(folder, 'metadata.db'))
for key in ('schema', 'application_id', 'user_version', 'preferences'):
    if ours[key] != theirs[key]:
        problems.append(f'the {key} differs from a library Calibre makes')
if ours['uuid'] == theirs['uuid'] or len(ours['uuid']) != 36:
    problems.append(f'the library id {ours["uuid"]!r} is not one of its own')

cache = open_library(library).new_api
titles = sorted(cache.field_for('title', book_id) for book_id in cache.all_book_ids())
if titles != ['Le Vicomte de Bragelonne: Tome premier']:
    problems.append(f'Calibre lists {titles}, not the book Cousoir added')
with open(os.path.join(data, 'import', 'Black Company 2.epub'), 'rb') as stream:
    mi = get_metadata(stream, 'epub')
ids, duplicates = cache.add_books([(mi, {'EPUB': os.path.join(data, 'import', 'Black Company 2.epub')})], run_hooks=False)
if len(ids) != 1:
    problems.append('Calibre could not add a book to it')
check = list(cache.backend.execute('PRAGMA integrity_check'))
if check != [('ok',)]:
    problems.append(f'the database fails its check: {check}')
cache.close()

for problem in problems:
    print(problem)
sys.exit(1 if problems else 0)
