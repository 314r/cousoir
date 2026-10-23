# Writes tests/data/annotations: metadata.db, the fixture library's database
# with highlights and bookmarks Calibre's own API saved on several books,
# and expected.json, what Calibre's annotation browser finds in it
# (all_annotations() and search_annotations() as gui2/library/annotations.py
# calls them, for all users or one), and users.json, the users it lists, for
# Cousoir's AnnotationSearch to find alike.
#
#   CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/annotations_oracle.py -- .

import json
import os
import shutil
import sys
import tempfile

from calibre.library import db as open_db

source = sys.argv[-1]
out = os.path.join(source, 'tests', 'data', 'annotations')
os.makedirs(out, exist_ok=True)
work = tempfile.mkdtemp()
library = os.path.join(work, 'library')
shutil.copytree(os.path.join(source, 'tests', 'data', 'library'), library)
cache = open_db(library).new_api


def highlight(uid, text, ts, spine, cfi, notes=None, toc=None, which='yellow'):
    a = {
        'type': 'highlight', 'uuid': uid, 'timestamp': ts, 'start_cfi': cfi, 'end_cfi': cfi + '0',
        'highlighted_text': text, 'spine_index': spine, 'spine_name': f'OEBPS/c{spine}.xhtml',
        'style': {'type': 'builtin', 'kind': 'color', 'which': which},
    }
    if notes:
        a['notes'] = notes
    if toc is not None:
        a['toc_family_titles'] = toc
    return a


def bookmark(uid, title, ts, pos):
    return {'type': 'bookmark', 'uuid': uid, 'title': title, 'timestamp': ts, 'pos': pos, 'pos_type': 'epubcfi'}


cache.merge_annotations_for_book(1, 'EPUB', [
    highlight('a1', "L'amour est une chose qui ne se décrit pas.", '2026-01-02T10:00:00.000Z', 3, '/4/2/6/1:12',
              notes='Le jazz, toujours.', toc=['Chapitre II']),
    highlight('a2', 'Colin aimait la musique de Duke Ellington.', '2026-01-01T09:00:00.000Z', 1, '/4/2/2/1:0',
              toc=['Chapitre I'], which='green'),
    highlight('a3', 'Les nénuphars poussaient dans la poitrine.', '2026-01-03T11:00:00.000Z', 3, '/4/2/2/1:5',
              toc=['Chapitre II', 'Une partie']),
    bookmark('a4', 'Le pianocktail', '2026-01-04T12:00:00.000Z', 'epubcfi(/6/4!/4/2/1:0)'),
    {'removed': True, 'type': 'highlight', 'uuid': 'a5', 'timestamp': '2026-01-05T12:00:00.000Z'},
])
cache.merge_annotations_for_book(2, 'EPUB', [
    highlight('b1', 'The Company is reading the Annals by the fire.', '2025-12-01T08:00:00.000Z', 2, '/4/2/4/1:0',
              notes='Croaker writes everything down.'),
    highlight('b2', 'Soldiers live. And wonder why.', '2025-12-02T08:00:00.000Z', 5, '/4/2/2/1:0', toc=[]),
])
# A Kobo's, its own user's, as Cousoir puts them in.
cache.merge_annotations_for_book(2, 'EPUB', [
    highlight('k1', 'The Annals are the memory of the Company.', '2025-12-03T08:00:00.000Z', 3, '/4/2/8/1:0',
              notes='Read on the Kobo.', toc=['Chapter Three'], which='red'),
], user_type='kobo', user='N418000000000')
cache.merge_annotations_for_book(4, 'EPUB', [
    highlight('c1', 'Reading books about the end of the world was his favourite pastime.', '2025-11-01T08:00:00.000Z',
              2, '/4/2/2/1:0', which='blue'),
])

queries = [
    {'text': '', 'stemming': True, 'type': ''},
    {'text': '', 'stemming': True, 'type': 'bookmark'},
    {'text': 'amour', 'stemming': True, 'type': ''},
    {'text': 'nenuphars', 'stemming': False, 'type': ''},
    {'text': 'reading', 'stemming': True, 'type': ''},
    {'text': 'reading', 'stemming': False, 'type': ''},
    {'text': 'jazz', 'stemming': True, 'type': ''},
    {'text': '"Duke Ellington"', 'stemming': True, 'type': ''},
    {'text': 'soldier*', 'stemming': False, 'type': 'highlight'},
    {'text': 'pianocktail', 'stemming': True, 'type': 'highlight'},
    {'text': '', 'stemming': True, 'type': '', 'user': ['kobo', 'N418000000000']},
    {'text': '', 'stemming': True, 'type': 'highlight', 'user': ['local', 'viewer']},
    {'text': 'annals', 'stemming': True, 'type': '', 'user': ['kobo', 'N418000000000']},
    {'text': 'annals', 'stemming': True, 'type': '', 'user': ['local', 'viewer']},
]
expected = []
for q in queries:
    user = tuple(q['user']) if 'user' in q else None
    if q['text']:
        found = cache.search_annotations(q['text'], use_stemming=q['stemming'], highlight_start='\x1d',
                                         highlight_end='\x1d', snippet_size=64, annotation_type=q['type'],
                                         restrict_to_book_ids=None, restrict_to_user=user, ignore_removed=True)
    else:
        found = cache.all_annotations(restrict_to_user=user, limit=4096, annotation_type=q['type'],
                                      ignore_removed=True, restrict_to_book_ids=None)
    expected.append({'query': q, 'results': [[r['id'], r['book_id'], r['format'], r['user_type'], r['user'],
                                              r['text'], r['annotation'].get('uuid')] for r in found]})
users = sorted([list(u) for u in cache.all_annotation_users()])
cache.close()
shutil.copyfile(os.path.join(library, 'metadata.db'), os.path.join(out, 'metadata.db'))
with open(os.path.join(out, 'users.json'), 'w', encoding='utf-8') as f:
    json.dump(users, f)
    f.write('\n')
with open(os.path.join(out, 'expected.json'), 'w', encoding='utf-8') as f:
    json.dump(expected, f, ensure_ascii=False, indent=1)
    f.write('\n')
shutil.rmtree(work)
