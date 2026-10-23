# Has Calibre read, searched and changed the highlights cousoir-cli put into
# a library from a Kobo (tests/golden/run.sh): the Kobo's own annotations
# user, its highlights where Calibre's viewer keeps a highlight, found by
# Calibre's search; and does Cousoir leave alone what Calibre changed when
# it syncs again. Prints what differs and exits with 1.
#
#   calibre-debug -e tools/oracle/kobo_highlights_check.py -- LIBRARY STAGE
#
# STAGE "synced" checks the library cousoir-cli synced, then changes it as
# Calibre would; "again" checks it once synced a second time.

import sys

from calibre.library import db as open_db

library, stage = sys.argv[-2], sys.argv[-1]
user = ('kobo', 'N418000000000')
cache = open_db(library).new_api
problems = []


def kobo_highlights():
    return {a['annotation']['uuid']: a for a in cache.all_annotations(restrict_to_user=user, ignore_removed=True)}


if stage == 'synced':
    if user not in cache.all_annotation_users():
        problems.append(f'Calibre knows no user {user}: {cache.all_annotation_users()}')
    found = kobo_highlights()
    expected = {'v1', 'v2', 'v3', 'v4', 'v5', 'v9', 'v10', 'w1'}
    if set(found) != expected:
        problems.append(f'the Kobo\'s highlights are {sorted(found)}, not {sorted(expected)}')
    for uuid, a in sorted(found.items()):
        annotation = a['annotation']
        for key in ('start_cfi', 'end_cfi', 'spine_index', 'spine_name', 'highlighted_text', 'style', 'timestamp'):
            if key not in annotation:
                problems.append(f'{uuid} has no {key}')
        if a['format'] != 'EPUB':
            problems.append(f'{uuid} is kept with {a["format"]}')
    if found.get('v4', {}).get('annotation', {}).get('notes') != 'Check it':
        problems.append('the note made on the Kobo is not v4\'s')
    hits = {a['annotation']['uuid'] for a in cache.search_annotations('note', restrict_to_user=user)}
    if 'v4' not in hits:
        problems.append(f'searching the Kobo\'s highlights for "note" finds {sorted(hits)}')
    book = next(iter(found.values()))['book_id'] if found else 0
    if book and cache.annotation_count_for_book(book) < len(expected):
        problems.append(f'Calibre counts {cache.annotation_count_for_book(book)} annotations for the book')
    # Calibre removes one and changes another, as its browser does.
    if found:
        cache.delete_annotations([found['v2']['id']])
        changed = dict(found['v5']['annotation'])
        changed['notes'] = 'Changed in Calibre'
        cache.update_annotations({found['v5']['id']: changed})
elif stage == 'again':
    found = kobo_highlights()
    if 'v2' in found:
        problems.append('the highlight Calibre removed came back')
    if found.get('v5', {}).get('annotation', {}).get('notes') != 'Changed in Calibre':
        problems.append(f'the note Calibre wrote reads {found.get("v5", {}).get("annotation", {}).get("notes")!r}')
    if 'v1' not in found:
        problems.append('a highlight the Kobo still has is gone')
cache.close()
for problem in problems:
    print(problem)
sys.exit(1 if problems else 0)
