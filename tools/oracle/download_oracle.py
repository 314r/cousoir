# What a metadata download finds and changes, for the golden test that
# compares Cousoir's download with that of Calibre's Edit metadata dialog,
# without going on the web:
#
#   calibre-debug -e tools/oracle/download_oracle.py -- identify <metadata_download.cpp> <folder> <found.json>
#   calibre-debug -e tools/oracle/download_oracle.py -- apply <library> <ops.json>
#   calibre-debug -e tools/oracle/download_oracle.py -- covers <metadata_download.cpp> <folder>
#
# identify runs Cousoir's identify job, the Python of metadata_download.cpp,
# in <folder> as Cousoir runs it, but for Calibre's single_identify(), which
# returns the books of found.json, as OPF the way it does, instead of asking
# the sources; the OPFs are kept in <folder>/opfs.json for apply.
#
# covers runs Cousoir's covers job in the identify job's <folder>, with
# Calibre's single_covers() saving two covers as it does instead of asking
# the sources, and checks that the job hands it the identify job's caches
# and ends with its result.
#
# apply makes the download ops of a cousoir-cli apply file, [{"download":
# {"book": 2, "found": <folder>, "result": 0, "fields": [...], "append":
# false}}], in Calibre's Edit metadata dialog, shown offscreen: the book
# read back from its OPF as the dialog reads it, the fields not chosen set
# to nothing as fetch_metadata() does for the ones Calibre is set to ignore,
# then update_from_mi() and apply_changes(). The summary is the exception:
# Calibre's editor rewrites the HTML it is given, which Cousoir keeps as it
# is, so it is set as update_from_mi() makes it, from the book's own.

import json
import os
import sys
from datetime import datetime
from io import BytesIO

mode = sys.argv[sys.argv.index('--') + 1] if '--' in sys.argv else sys.argv[1]
args = sys.argv[sys.argv.index(mode) + 1:]


def metadata(entry):
    from calibre.ebooks.metadata.book.base import Metadata
    from calibre.utils.date import parse_date

    mi = Metadata(entry.get('title'), entry.get('authors'))
    for field in ('publisher', 'series', 'series_index', 'tags', 'rating', 'comments', 'languages'):
        if field in entry:
            setattr(mi, field, entry[field])
    if 'pubdate' in entry:
        mi.pubdate = parse_date(entry['pubdate'], assume_utc=True)
    if 'identifiers' in entry:
        mi.set_identifiers(entry['identifiers'])
    return mi


# The sources' caches single_identify() returns, as dump_caches() makes them.
CACHES = {'Google': {'isbn_to_identifier': {'9780812521399': 'xE4_AAAA'},
                     'identifier_to_cover': {'xE4_AAAA': 'https://books.google.com/cover.jpg'}}}


def job_script(cpp, name):
    with open(cpp, encoding='utf-8') as f:
        source = f.read()
    start = source.index(name + '[] = R"(') + len(name + '[] = R"(')
    return source[start:source.index(')";', start)]


def identify(cpp, folder, found_path):
    import calibre.ebooks.metadata.sources.update as update
    import calibre.ebooks.metadata.sources.worker as worker
    from calibre.ebooks.metadata.opf2 import metadata_to_opf
    from calibre.utils.logging import GUILog

    with open(found_path, encoding='utf-8') as f:
        found = json.load(f)
    script = job_script(cpp, 'identifyScript')

    opfs = []

    def single_identify(title, authors, identifiers):
        log = GUILog()
        log('Asked for', title, authors, identifiers)
        results = [metadata(entry) for entry in found]
        opfs.extend(metadata_to_opf(r) for r in results)
        return list(opfs), [bool(entry.get('has_cover')) for entry in found], CACHES, log.dump()

    worker.single_identify = single_identify
    update.update_sources = lambda wait_for_completion=False: True
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'job.json'), 'w', encoding='utf-8') as f:
        json.dump({'kind': 'identify', 'title': 'Asked', 'authors': ['Someone'], 'identifiers': [], 'timeout': 30}, f)
    sys.argv = [os.path.join(folder, 'download.py'), '--', folder]
    exec(compile(script, 'download.py', 'exec'), {'__name__': '__main__'})
    with open(os.path.join(folder, 'opfs.json'), 'w', encoding='utf-8') as f:
        json.dump([opf.decode('utf-8') for opf in opfs], f)


def apply(library, ops_path):
    from calibre.gui2 import Application

    app = Application([])  # noqa: F841
    from calibre.ebooks.metadata.book.base import Metadata
    from calibre.ebooks.metadata.opf2 import OPF
    from calibre.gui2.metadata.single import MetadataSingleDialog
    from calibre.library import db as open_library
    from calibre.library.comments import merge_comments
    from calibre.utils.date import local_tz
    from calibre.utils.localization import _

    base = os.path.dirname(os.path.abspath(ops_path))
    with open(ops_path, encoding='utf-8') as f:
        ops = [op['download'] for op in json.load(f) if 'download' in op]
    db = open_library(library)
    # The books dirtied before, by an earlier run, Calibre would write again
    # only when it next opens the library, as it would Cousoir's.
    start = db.new_api.dirtied_sequence
    # Cousoir's fields, by the Metadata fields they stand for.
    fields = {'title': ['title'], 'authors': ['authors', 'author_sort'], 'rating': ['rating'],
              'publisher': ['publisher'], 'tags': ['tags'], 'identifiers': ['identifiers'],
              'pubdate': ['pubdate'], 'series': ['series', 'series_index'], 'languages': ['languages'],
              'comments': ['comments']}
    for op in ops:
        with open(os.path.join(base, op['found'], 'opfs.json'), encoding='utf-8') as f:
            opf = json.load(f)[op['result']]
        mi = OPF(BytesIO(opf.encode('utf-8')), basedir=os.getcwd(), populate_spine=False).to_book_metadata()
        dummy = Metadata(_('Unknown'))
        for field, names in fields.items():
            if field not in op['fields']:
                for name in names:
                    setattr(mi, name, getattr(dummy, name))
        comments, mi.comments = mi.comments, dummy.comments
        pd = mi.pubdate
        if pd is not None:
            mi.pubdate = datetime(pd.year, pd.month, pd.day, tzinfo=local_tz)
        book = op['book']
        d = MetadataSingleDialog(db)
        d.id_list = [book]
        d.current_row = 0
        d.set_current_callback = None
        d.do_one(apply_changes=False)
        d.update_from_mi(mi, merge_comments=op.get('append', False))
        if not d.apply_changes():
            raise SystemExit(f'The dialog did not save book {book}')
        d.break_cycles()
        if comments and comments.strip():
            old = db.new_api.field_for('comments', book)
            if op.get('append') and old:
                comments = merge_comments(old, comments)
            db.new_api.set_field('comments', {book: comments})
    # The metadata.opf files written, the books left dirtied, as Cousoir
    # leaves them for Calibre to write again.
    cache = db.new_api
    changed = [book_id for book_id, sequence in cache.dirtied_cache.items() if sequence >= start]
    cache.dump_metadata(book_ids=changed, remove_from_dirtied=False)
    db.close()


def covers(cpp, folder):
    import calibre.ebooks.metadata.sources.worker as worker
    from calibre.utils.logging import GUILog

    script = job_script(cpp, 'coversScript')
    with open(os.path.join(os.path.dirname(os.path.abspath(cpp)), 'tests/data/edits/large.jpg'), 'rb') as f:
        image = f.read()
    asked = []

    def single_covers(title, authors, identifiers, caches, tdir):
        asked.append(caches)
        for name in ('Google,,1200,,1800,,jpeg', 'Amazon.com{0},,300,,450,,jpeg'):
            with open(os.path.join(tdir, name + '.cover'), 'wb') as f:
                f.write(image)
            os.mkdir(os.path.join(tdir, name + '.cover.done'))
        return GUILog().dump()

    worker.single_covers = single_covers
    with open(os.path.join(folder, 'job.json'), 'w', encoding='utf-8') as f:
        json.dump({'kind': 'covers', 'title': 'Asked', 'authors': ['Someone'], 'identifiers': [['isbn', '1']],
                   'timeout': 30}, f)
    sys.argv = [os.path.join(folder, 'download.py'), '--', folder]
    # Twice, as the dialog does for a book found and then another: the job
    # finds the folder of covers emptied by Cousoir in between.
    for _ in range(2):
        exec(compile(script, 'download.py', 'exec'), {'__name__': '__main__'})
        if asked[-1] != CACHES:
            raise SystemExit(f'The covers job was given {asked[-1]!r}, not the identify job\'s caches')
        with open(os.path.join(folder, 'result.json'), encoding='utf-8') as f:
            if json.load(f) != {'covers': True}:
                raise SystemExit('The covers job wrote no result')
        names = sorted(os.listdir(os.path.join(folder, 'covers')))
        if len(names) != 4:
            raise SystemExit(f'The covers job saved {names}')
        import shutil
        shutil.rmtree(os.path.join(folder, 'covers'))
        os.remove(os.path.join(folder, 'result.json'))


if mode == 'identify':
    identify(*args[:3])
elif mode == 'covers':
    covers(*args[:2])
elif mode == 'apply':
    apply(*args[:2])
else:
    raise SystemExit(f'Unknown mode {mode}')
