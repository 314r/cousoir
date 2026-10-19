#!/usr/bin/env python3
# Does to a fake Kobo what a Kobo does once unplugged: takes in the book
# files sent to it, as rows of .kobo/KoboReader.sqlite with the title and
# authors of metadata.calibre, and makes their covers in its cache; forgets
# the books whose files are gone.
#
#   kobo_import.py <kobo>

import json
import os
import sqlite3
import sys

root = os.path.abspath(sys.argv[1])
ONBOARD = 'file:///mnt/onboard/'
TYPES = {'.kepub.epub': 'application/x-kobo-epub+zip', '.epub': 'application/epub+zip', '.pdf': 'application/pdf'}


def qhash(text):
    h = 0
    for x in text.encode('utf-8'):
        h = (h << 4) + x
        h ^= (h & 0xF0000000) >> 23
        h &= 0x0FFFFFFF
    return h


def image_id(content_id):
    for c in '/ :.':
        content_id = content_id.replace(c, '_')
    return content_id


try:
    with open(os.path.join(root, 'metadata.calibre'), encoding='utf-8') as f:
        known = {book['lpath']: book for book in json.load(f)}
except FileNotFoundError:
    known = {}

con = sqlite3.connect(os.path.join(root, '.kobo', 'KoboReader.sqlite'))
present = set()
for folder, dirs, files in os.walk(root):
    dirs[:] = [d for d in dirs if not d.startswith('.')]
    for name in sorted(files):
        mime = next((m for ext, m in TYPES.items() if name.lower().endswith(ext)), None)
        if mime is None:
            continue
        lpath = os.path.relpath(os.path.join(folder, name), root).replace(os.sep, '/')
        content_id = ONBOARD + lpath
        present.add(content_id)
        if con.execute('SELECT 1 FROM content WHERE ContentID = ?', (content_id,)).fetchone():
            continue
        book = known.get(lpath, {})
        image = image_id(content_id)
        con.execute(
            'INSERT INTO content (ContentID, ContentType, MimeType, BookID, ImageId, Title, Attribution, ___UserID,'
            ' ___FileSize, Accessibility, IsDownloaded, ReadStatus) VALUES (?, 6, ?, NULL, ?, ?, ?, ?, ?, -1, ?, 0)',
            (content_id, mime, image, book.get('title', os.path.splitext(name)[0]),
             ' & '.join(book.get('authors', [])) or None, '', os.path.getsize(os.path.join(root, lpath)), 'true'))
        h = qhash(image)
        covers = os.path.join(root, '.kobo-images', str(h & 0xFF), str((h & 0xFF00) >> 8))
        os.makedirs(covers, exist_ok=True)
        for ending in (' - N3_LIBRARY_FULL.parsed', ' - N3_LIBRARY_GRID.parsed', ' - N3_FULL.parsed'):
            with open(os.path.join(covers, image + ending), 'wb') as f:
                f.write(b'cover')
for (content_id,) in con.execute("SELECT ContentID FROM content WHERE ContentID LIKE 'file:///mnt/onboard/%'").fetchall():
    if content_id not in present:
        con.execute('DELETE FROM content WHERE ContentID = ?', (content_id,))
con.commit()
con.close()
