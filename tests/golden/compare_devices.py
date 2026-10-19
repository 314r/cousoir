#!/usr/bin/env python3
# Compares two fake readers, Kobos or Kindles, after the same books were sent
# to or removed from them, one by Calibre and one by Reliure:
#
#   compare_devices.py [--lpaths-only] <calibre kobo> <reliure kobo>
#
# - every file but a Kobo's database, which Calibre edits and Reliure leaves
#   alone;
# - the book files by their content, without the time Calibre stamps in
#   each: an EPUB's entries, in any order, without its dcterms:modified, a
#   PDF without its XMP MetadataDate; an APNX without the id Calibre draws;
# - metadata.calibre as parsed JSON, book by book, without the temporary
#   file each book's cover was read from, whose path Calibre keeps, or the
#   size of an EPUB, which its time changes; with --lpaths-only, only which
#   books it has;
# - driveinfo.calibre without the time of the connection, the uuid drawn for
#   a new device or the folder a Kobo is mounted on.
#
# Prints each difference and exits with 1 when there is one.

import hashlib
import json
import os
import re
import sys
import zipfile

lpaths_only = '--lpaths-only' in sys.argv
calibre_kobo, reliure_kobo = [a for a in sys.argv[1:] if not a.startswith('--')]
problems = []


def files(root):
    found = {}
    for folder, dirs, names in os.walk(root):
        for name in names:
            rel = os.path.relpath(os.path.join(folder, name), root)
            if not rel.startswith(os.path.join('.kobo', 'KoboReader.sqlite')):
                found[rel] = os.path.join(folder, name)
    return found


def content(path):
    with open(path, 'rb') as f:
        raw = f.read()
    if raw.startswith(b'PK'):
        entries = []
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                data = archive.read(info)
                if info.filename.endswith('.opf'):
                    data = re.sub(rb'(<meta property="dcterms:modified"[^>]*>)[^<]*', rb'\1<now>', data)
                entries.append((info.filename, hashlib.sha256(data).hexdigest()))
        # Calibre zips the files in no set order but the first.
        return entries[:1] + sorted(entries[1:])
    if raw.startswith(b'%PDF'):
        raw = re.sub(rb'(<xmp:MetadataDate>)[^<]*', rb'\1<now>', raw)
    if path.endswith('.apnx'):
        raw = re.sub(rb'("contentGuid":")[0-9a-f]+', rb'\1<id>', raw)
    return hashlib.sha256(raw).hexdigest()


def metadata(path, root):
    with open(path, encoding='utf-8') as f:
        books = json.load(f)
    found = {}
    for book in books:
        book = dict(book)
        book['cover'] = '<cover>' if book.get('cover') else book.get('cover')
        if book['lpath'].lower().endswith('.epub'):
            book['size'] = '<size>'
        found[book['lpath']] = book
    return found


def driveinfo(path, root):
    with open(path, encoding='utf-8') as f:
        info = json.load(f)
    info['date_last_connected'] = '<now>'
    info['device_store_uuid'] = '<uuid>'
    if 'prefix' in info:
        info['prefix'] = info['prefix'].replace(os.path.abspath(root), '@DEVICE@')
    return info


fa, fb = files(calibre_kobo), files(reliure_kobo)
for rel in sorted(set(fa) | set(fb)):
    if rel not in fa or rel not in fb:
        problems.append(f'{rel} only on the {"Calibre" if rel in fa else "Reliure"} device')
    elif rel == 'metadata.calibre':
        ma, mb = metadata(fa[rel], calibre_kobo), metadata(fb[rel], reliure_kobo)
        for lpath in sorted(set(ma) | set(mb)):
            if lpath not in ma or lpath not in mb:
                problems.append(f'metadata.calibre: {lpath} only in {"Calibre" if lpath in ma else "Reliure"}\'s')
                continue
            if lpaths_only:
                continue
            for key in sorted(set(ma[lpath]) | set(mb[lpath])):
                if ma[lpath].get(key) != mb[lpath].get(key):
                    problems.append(f'metadata.calibre: {lpath}: {key} differs:\n'
                                    f'    Calibre: {str(ma[lpath].get(key))[:120]}\n'
                                    f'    Reliure: {str(mb[lpath].get(key))[:120]}')
    elif rel == 'driveinfo.calibre':
        da, db = driveinfo(fa[rel], calibre_kobo), driveinfo(fb[rel], reliure_kobo)
        if da != db:
            problems.append(f'driveinfo.calibre differs:\n    Calibre: {da}\n    Reliure: {db}')
    elif content(fa[rel]) != content(fb[rel]):
        problems.append(f'{rel} differs')

for problem in problems:
    print(problem)
print(f'{len(fa)} files compared, {len(problems)} differences')
sys.exit(1 if problems else 0)
