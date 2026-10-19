# Converts books as Calibre's convert action does when a format is picked
# for them, for the golden test that compares Reliure's conversions with
# Calibre's:
#
#   calibre-debug -e tools/oracle/convert_books.py -- <library> <changes.json>
#
# The file is reliure-cli apply's; its { "convert": { "book": 1, "format":
# "KEPUB" } } go, in order, through convert_single_ebook() of gui2/tools.py
# with auto_conversion, which saves the book's settings, the job,
# gui_convert_override(), in a worker process of its own as the job manager
# runs it, and what book_converted() of gui2/actions/convert.py does with its
# file. The dialog needs a QApplication, without a window here. The other
# changes of the file are left out.

import json
import sys

from calibre.gui2 import Application

app = Application([], headless=True)

from calibre.customize.ui import run_plugins_on_postconvert  # noqa: E402
from calibre.gui2.tools import convert_single_ebook  # noqa: E402
from calibre.library import db as open_library  # noqa: E402
from calibre.utils.config import tweaks  # noqa: E402
from calibre.utils.ipc.simple_worker import fork_job  # noqa: E402

library, changes_path = sys.argv[-2:]
with open(changes_path, encoding='utf-8') as f:
    changes = json.load(f)

db = open_library(library)


class NoPageCounts:
    # Calibre counts the pages of the books it changed in the background,
    # and corrects on the way the sizes of files changed outside it: not
    # part of converting, and left to Calibre for Reliure's changes too.
    def queue_scan(self, book_id=0):
        pass

    def shutdown(self):
        pass

    def wait_for_worker_shutdown(self, timeout=None):
        pass


db.new_api.maintain_page_counts.shutdown()
db.new_api.maintain_page_counts = NoPageCounts()
for change in changes:
    if 'convert' not in change:
        continue
    book_id, fmt = change['convert']['book'], change['convert']['format'].lower()
    jobs, changed, bad = convert_single_ebook(None, db, [book_id], auto_conversion=True, out_format=fmt,
                                              show_no_format_warning=False)
    if bad:
        raise SystemExit(f'Book {book_id} has no format Calibre converts from')
    for func, args, desc, out_fmt, job_book, temp_files in jobs:
        parts = set(func.partition(':')[2].split(';'))
        fork_job('calibre.gui2.convert.gui_conversion', 'gui_convert_override', args=args, timeout=3600)
        if 'same_fmt' in parts and tweaks['save_original_format']:
            db.save_original_format(job_book, out_fmt, notify=False)
        with open(temp_files[-1].name, 'rb') as data:
            db.add_format(job_book, out_fmt, data, index_is_id=True)
        run_plugins_on_postconvert(db, job_book, out_fmt)
db.close()
