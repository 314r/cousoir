# Has Calibre read, search, change and bring back the notes cousoir-cli wrote
# into a library (tests/golden/run.sh): what Calibre's own Notes class and
# its full text index make of them. Prints what differs and exits with 1.
#
#   calibre-debug -e tools/oracle/notes_check.py -- LIBRARY

import sys

from calibre.library import db as open_db

cache = open_db(sys.argv[-1]).new_api
backend = cache.backend
problems = []


def item(field, name):
    for item_id, value in cache.get_id_map(field).items():
        if value == name:
            return item_id
    raise SystemExit(f'No {field} named {name}')


def integrity(when):
    try:
        backend.execute("INSERT INTO notes_db.notes_fts(notes_fts) VALUES('integrity-check')")
        backend.execute("INSERT INTO notes_db.notes_fts_stemmed(notes_fts_stemmed) VALUES('integrity-check')")
    except Exception as e:
        problems.append(f'the notes index is damaged {when}: {e}')


fantasy, tor, contes = item('tags', 'Fantasy'), item('publisher', 'Tor'), item('series', 'Contes')
integrity('as Cousoir left it')
if cache.notes_for('tags', fantasy) != '<p>Other worlds, élégantes.</p>':
    problems.append(f'the tag note reads {cache.notes_for("tags", fantasy)!r}')
if cache.notes_for('publisher', tor) != '<p>New York.</p>':
    problems.append(f'the publisher note reads {cache.notes_for("publisher", tor)!r}')
if cache.notes_for('series', contes):
    problems.append('the series note is still there')
found = {(r['field'], r['item_id']) for r in cache.search_notes('elegantes')}
if ('tags', fantasy) not in found:
    problems.append(f'searching the notes for "elegantes" finds {found}')
# Calibre changes and removes what Cousoir wrote, with its own tokenizer.
cache.set_notes_for('tags', fantasy, '')
cache.set_notes_for('publisher', tor, '<p>Manhattan.</p>', searchable_text='Manhattan.')
integrity('once Calibre changed the notes Cousoir wrote')
# And brings back the note Cousoir removed, from where Cousoir kept it.
if cache.unretire_note_for('series', contes) < 0 or 'Tales for children' not in (cache.notes_for('series', contes) or ''):
    problems.append('Calibre cannot bring back the series note Cousoir removed')
integrity('once Calibre brought a note back')
cache.close()
for problem in problems:
    print(problem)
sys.exit(1 if problems else 0)
