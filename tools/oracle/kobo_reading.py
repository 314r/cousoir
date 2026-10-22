# Prints what Calibre's Kobo driver reads of the reading of each book of a
# Kobo, by its ContentID, in the shape `cousoir-cli device` prints it:
#
#   calibre-debug -e tools/oracle/kobo_reading.py -- <kobo folder> <output.json>
#
# Calibre's Bookmark, of src/calibre/devices/kobo/bookmark.py, reads the
# Kobo's database for each book the Kobo took in, as get_annotations() of
# its driver has it read; its path, of which it only takes the time, is the
# database's here. None is printed as an empty text.

import json
import os
import sqlite3
import sys

from calibre.devices.kobo.bookmark import Bookmark

root, out = sys.argv[-2:]
database = os.path.join(root, '.kobo', 'KoboReader.sqlite')
connection = sqlite3.connect(database)
connection.row_factory = sqlite3.Row
books = {}
ids = [row[0] for row in connection.execute('SELECT ContentID FROM content WHERE BookID IS NULL AND ContentType IN (6, 10)')]
for content_id in ids:
    bookmark = Bookmark(connection, content_id, database, 0, 'epub', 'epub')
    notes = []
    for location, note in sorted(bookmark.user_notes.items()):
        notes.append({
            'location': location,
            'type': note['type'],
            'text': note['text'] or '',
            'annotation': note['annotation'] or '',
            'chapter': note['chapter'],
            'chapter_title': note['chapter_title'] or '',
            'chapter_progress': note['chapter_progress'],
        })
    books[content_id] = {
        'percent_read': bookmark.percent_read,
        'last_read': bookmark.last_read or None,
        'notes': notes,
    }
with open(out, 'w', encoding='utf-8') as f:
    json.dump(books, f, ensure_ascii=False, indent=2, sort_keys=True)
