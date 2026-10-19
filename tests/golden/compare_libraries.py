#!/usr/bin/env python3
# Compares two copies of a Calibre library after the same edits, one written
# by Calibre and one by Reliure:
#
#   compare_libraries.py [--files-only] [--added-from N] <calibre copy> <reliure copy>
#
# - every table of metadata.db but the preferences Calibre updates when it
#   opens a library, with last_modified, the time of the write, left out, and
#   for books added, from id N, the time and the random uuid they got;
# - the files: metadata.opf as parsed XML, without the producer, which names
#   the program; cover.jpg by its size and pixels, as two JPEG encoders may
#   differ by a few bits; anything else byte for byte.
#
# Prints each difference and exits with 1 when there is one.

import hashlib
import os
import sqlite3
import subprocess
import sys
import xml.etree.ElementTree as ET

files_only = '--files-only' in sys.argv
added_from = int(sys.argv[sys.argv.index('--added-from') + 1]) if '--added-from' in sys.argv else None
arguments = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] != '--added-from']
calibre_copy, reliure_copy = arguments[:2]
problems = []

SKIPPED_TABLES = {'preferences'}


def tables(path):
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    con.create_function('title_sort', -1, lambda *a: a[0])
    names = [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
    result = {}
    for name in names:
        if name in SKIPPED_TABLES or name.startswith('annotations_fts'):
            continue
        cursor = con.execute(f'SELECT * FROM "{name}"')
        columns = [d[0] for d in cursor.description]
        rows = []
        for row in cursor:
            row = list(row)
            if name == 'books':
                row[columns.index('last_modified')] = '<now>'
                if added_from is not None and row[columns.index('id')] >= added_from:
                    row[columns.index('timestamp')] = '<now>'
                    row[columns.index('uuid')] = '<uuid>'
            if name == 'books_pages_link':
                # The row's creation time.
                row[columns.index('timestamp')] = '<now>'
            if name == 'identifiers':
                # Calibre inserts a book's identifiers from a set, in no
                # particular order.
                row[columns.index('id')] = '<id>'
            rows.append(tuple(row))
        result[name] = (columns, sorted(rows, key=repr))
    con.close()
    return result


a = {} if files_only else tables(os.path.join(calibre_copy, 'metadata.db'))
b = {} if files_only else tables(os.path.join(reliure_copy, 'metadata.db'))
if 'books_pages_link' in a and 'books_pages_link' in b:
    # Calibre counts a book's pages in the background; Reliure leaves the
    # books it changed for Calibre to count: either is right, on either side.
    columns = b['books_pages_link'][0]
    scan = columns.index('needs_scan')
    to_scan = {row[0] for side in (a, b) for row in side['books_pages_link'][1] if row[scan] == 1}

    def counted(row):
        if row[0] not in to_scan:
            return row
        return (row[0],) + ('<scan>',) * (len(row) - 3) + (row[-2], '<scan>')

    for side in (a, b):
        cols, rows = side['books_pages_link']
        side['books_pages_link'] = (cols, sorted((counted(r) for r in rows), key=repr))
for name in sorted(set(a) | set(b)):
    if name not in a or name not in b:
        problems.append(f'table {name} only in one copy')
        continue
    if a[name] != b[name]:
        ra, rb = set(a[name][1]), set(b[name][1])
        problems.append(f'table {name} differs:\n    Calibre only: {sorted(ra - rb, key=repr)[:6]}\n'
                        f'    Reliure only: {sorted(rb - ra, key=repr)[:6]}')


def files(root):
    found = {}
    for folder, dirs, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, root)
            if rel == 'metadata.db' or rel.startswith('.calnotes'):
                continue
            found[rel] = path
    return found


def added_book(path):
    # A book folder is "Title (id)", its trash entry ".caltrash/b/id".
    folder = os.path.basename(os.path.dirname(path))
    parent = os.path.basename(os.path.dirname(os.path.dirname(path)))
    if added_from is None:
        return False
    try:
        if parent == 'b' and folder.isdigit():
            return int(folder) >= added_from
        if folder.endswith(')') and '(' in folder:
            return int(folder.rpartition('(')[2][:-1]) >= added_from
    except ValueError:
        pass
    return False


def opf(path):
    tree = ET.parse(path)
    masked = added_book(path)
    ns = {'dc': 'http://purl.org/dc/elements/1.1/', 'opf': 'http://www.idpf.org/2007/opf'}
    metadata = tree.getroot().find('opf:metadata', ns)
    items = []
    identifiers = []
    for element in metadata:
        if element.get('{http://www.idpf.org/2007/opf}role') == 'bkp':
            continue
        text = (element.text or '').strip()
        attributes = dict(element.attrib)
        if masked and element.get('{http://www.idpf.org/2007/opf}scheme') == 'uuid':
            text = '<uuid>'
        if masked and element.get('name') == 'calibre:timestamp':
            attributes['content'] = '<now>'
        item = (element.tag, sorted(attributes.items()), text)
        # The identifiers come in database order, which differs as above.
        if element.tag.endswith('identifier') and element.get('id') is None:
            identifiers.append(item)
        else:
            items.append(item)
    items.extend(sorted(identifiers))
    guide = [(r.tag, sorted(r.attrib.items())) for r in tree.getroot().find('opf:guide', ns)]
    return items, guide


def image(path):
    size = subprocess.run(['magick', 'identify', '-format', '%wx%h', path], capture_output=True, text=True).stdout
    return size


fa, fb = files(calibre_copy), files(reliure_copy)
for rel in sorted(set(fa) | set(fb)):
    if rel not in fa or rel not in fb:
        problems.append(f'file {rel} only in the {"Calibre" if rel in fa else "Reliure"} copy')
        continue
    if os.path.basename(rel) == 'metadata.opf':
        if opf(fa[rel]) != opf(fb[rel]):
            problems.append(f'{rel} differs:\n    Calibre: {opf(fa[rel])}\n    Reliure: {opf(fb[rel])}')
    elif os.path.basename(rel) == 'cover.jpg':
        if image(fa[rel]) != image(fb[rel]):
            problems.append(f'{rel}: {image(fa[rel])} against {image(fb[rel])}')
        else:
            diff = subprocess.run(['magick', 'compare', '-metric', 'MAE', fa[rel], fb[rel], 'null:'],
                                  capture_output=True, text=True).stderr
            mean = float(diff.split('(')[1].split(')')[0]) if '(' in diff else 0.0
            if mean > 0.01:
                problems.append(f'{rel}: covers differ by {mean:.4f}')
    else:
        da = hashlib.sha256(open(fa[rel], 'rb').read()).hexdigest()
        db = hashlib.sha256(open(fb[rel], 'rb').read()).hexdigest()
        if da != db:
            problems.append(f'{rel} differs')

for problem in problems:
    print(problem)
print(f'{len(a)} tables and {len(fa)} files compared, {len(problems)} differences')
sys.exit(1 if problems else 0)
