# Has Calibre read, search and keep up the full text index cousoir-cli made
# and filled in a library (tests/golden/run.sh), and Cousoir search what
# Calibre's own workers indexed in it. Prints what differs and exits with 1.
#
#   calibre-debug -e tools/oracle/text_index_check.py -- LIBRARY COUSOIR_CLI TESTS_DATA

import json
import os
import subprocess
import sys
import time

from calibre.library import db as open_db

library, cli, data = sys.argv[-3:]
cache = open_db(library).new_api
backend = cache.backend
problems = []


def integrity(when):
    for table in ('books_fts', 'books_fts_stemmed'):
        try:
            backend.execute(f"INSERT INTO fts_db.{table}({table}) VALUES('integrity-check')")
        except Exception as e:
            problems.append(f'{table} is damaged {when}: {e}')


def calibre_finds(query):
    return sorted((r['book_id'], r['format'].upper()) for r in cache.fts_search(query, return_text=False))


# calibre-debug points the libraries at Calibre's own Qt.
environment = {k: v for k, v in os.environ.items() if k not in ('LD_LIBRARY_PATH', 'CALIBRE_QT_PREFIX')}


def cousoir_finds(query):
    run = subprocess.run([cli, 'search-text', '--json', '--library', library, query], capture_output=True, text=True,
                         env=environment)
    if run.returncode not in (0, 1) or (run.returncode == 1 and not run.stderr.startswith("No book's text")):
        problems.append(f'cousoir-cli search-text {query!r} failed: {run.stderr.strip()}')
        return []
    rows = json.loads(run.stdout) if run.stdout.strip() else []
    return sorted((r['book'], r['format'].upper()) for r in rows)


def same(query, expected, when):
    theirs = calibre_finds(query)
    ours = cousoir_finds(query)
    if theirs != ours:
        problems.append(f'{query!r} {when}: Calibre finds {theirs}, Cousoir {ours}')
    if theirs != expected:
        problems.append(f'{query!r} {when}: Calibre finds {theirs}, not {expected}')


# On, as Calibre opens a library whose search is on; enabling it again
# would index every format anew.
if not cache.is_fts_enabled():
    problems.append('Calibre does not see its full text search on')
integrity('as Cousoir left it')
left = cache.fts_indexing_progress()[0]
if left:
    problems.append(f'Calibre still has {left} formats to index')
# The AZW3 of book 2 was removed and an EPUB added to book 6 by
# cousoir-cli apply, then indexed by cousoir-cli index-text.
same('portents', [(2, 'EPUB')], 'as Cousoir indexed')
same('cuisinier', [(1, 'EPUB')], 'as Cousoir indexed')
same('endnote', [(6, 'EPUB')], 'as Cousoir indexed')
same('manuel OR shadows', [(3, 'EPUB'), (5, 'PDF')], 'as Cousoir indexed')

# Calibre's own changes and workers, on Cousoir's index.
cache.add_format(3, 'AZW3', f'{data}/reader/voyage.azw3')
cache.remove_formats({1: ('EPUB',)})
cache.start_fts_pool()
start = time.monotonic()
while cache.fts_indexing_progress()[0] and time.monotonic() - start < 120:
    time.sleep(0.5)
if cache.fts_indexing_progress()[0]:
    problems.append('Calibre did not index the format it added')
integrity('after Calibre changed it')
same('thunder', [(3, 'AZW3')], 'as Calibre indexed')
same('cuisinier', [], 'after Calibre removed the format')
cache.close()

for problem in problems:
    print(problem)
sys.exit(1 if problems else 0)
