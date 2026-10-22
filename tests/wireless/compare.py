# Compares what two wireless device servers, Calibre's and Cousoir's, told
# the same reader app (tests/wireless/fake_koreader.py), message by message,
# and the books the app kept after: their metadata and their files. Prints
# the differences and exits with 1 when there are any.
#
#   python3 -I tests/wireless/compare.py [--files-by-name] CALIBRE_TRANSCRIPT CALIBRE_INBOX COUSOIR_TRANSCRIPT COUSOIR_INBOX
#
# Left out: what changes from one run to the next (times, the challenge, the
# uuid given the app, Calibre's temporary cover files), the keep-alive NOOPs,
# whose number depends on timing, and the thumbnails' JPEG data, whose
# encoders differ (their sizes are compared). The books' files compare by
# their content: an EPUB by its entries, the OPF's modification time aside;
# with --files-by-name, by their names only, and the lengths sent aside.

import difflib
import hashlib
import json
import os
import re
import sys
import zipfile


def normalize_book(book):
    book = dict(book)
    if BY_NAME:
        book['size'] = None
    thumbnail = book.get('thumbnail')
    if isinstance(thumbnail, list) and len(thumbnail) == 3:
        book['thumbnail'] = [thumbnail[0], thumbnail[1], '<jpeg>']
    if book.get('cover'):
        book['cover'] = '<cover>'
    return book


BY_NAME = '--files-by-name' in sys.argv


def normalize(transcript):
    result = []
    for message in transcript:
        op, arg = message['op'], message.get('arg')
        if op == 'NOOP' and arg == {}:
            continue
        if op == 'BINARY':
            result.append({'op': op, 'length': None if BY_NAME else message['length']})
            continue
        arg = json.loads(json.dumps(arg))
        if op == 'GET_INITIALIZATION_INFO':
            arg['passwordChallenge'] = '<challenge>' if arg.get('passwordChallenge') else ''
            arg.pop('currentLibraryName', None)
        elif op == 'SET_CALIBRE_DEVICE_INFO':
            arg['date_last_connected'] = '<now>'
            arg['device_store_uuid'] = '<uuid>'
        elif op == 'SEND_BOOK':
            arg['metadata'] = normalize_book(arg['metadata'])
            if BY_NAME:
                arg['length'] = None
        elif op == 'SEND_BOOK_METADATA':
            arg['data'] = normalize_book(arg['data'])
        result.append({'op': op, 'arg': arg})
    return result


OPF_MODIFIED = re.compile(r'(<meta property="dcterms:modified">)[^<]*|(<meta name="calibre:timestamp" content=")[^"]*')


def file_content(path):
    if path.lower().endswith(('.epub', '.kepub')) and zipfile.is_zipfile(path):
        entries = {}
        with zipfile.ZipFile(path) as z:
            for name in sorted(z.namelist()):
                data = z.read(name)
                if name.endswith('.opf'):
                    data = OPF_MODIFIED.sub(lambda m: (m.group(1) or m.group(2)) + '<now>', data.decode('utf-8')).encode('utf-8')
                    entries[name] = data.decode('utf-8').splitlines()
                else:
                    entries[name] = hashlib.sha1(data).hexdigest()
        return entries
    with open(path, 'rb') as f:
        return hashlib.sha1(f.read()).hexdigest()


def inbox(folder):
    with open(os.path.join(folder, 'metadata.calibre'), encoding='utf-8') as f:
        books = sorted((normalize_book(b) for b in json.load(f)), key=lambda b: b.get('lpath', ''))
    files = {}
    for root, _, names in os.walk(folder):
        for name in names:
            if name in ('metadata.calibre', 'driveinfo.calibre'):
                continue
            path = os.path.join(root, name)
            files[os.path.relpath(path, folder)] = None if BY_NAME else file_content(path)
    return {'books': books, 'files': files}


def text(value):
    return json.dumps(value, indent=1, sort_keys=True, ensure_ascii=False).splitlines()


def main():
    calibre_transcript, calibre_inbox, cousoir_transcript, cousoir_inbox = [a for a in sys.argv[1:] if not a.startswith('--')]
    with open(calibre_transcript, encoding='utf-8') as f:
        expected = normalize(json.load(f))
    with open(cousoir_transcript, encoding='utf-8') as f:
        got = normalize(json.load(f))
    differences = list(difflib.unified_diff(text(expected), text(got), 'calibre', 'cousoir', lineterm='', n=2))
    differences += list(difflib.unified_diff(text(inbox(calibre_inbox)), text(inbox(cousoir_inbox)),
                                             'calibre inbox', 'cousoir inbox', lineterm='', n=2))
    for line in differences:
        print(line)
    return 1 if differences else 0


if __name__ == '__main__':
    sys.exit(main())
