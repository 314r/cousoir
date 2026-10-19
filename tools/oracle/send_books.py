# Connects a Kobo to Calibre's GUI and sends books to it or removes books
# from it, as the GUI does, for the golden test that compares Reliure's
# sending with Calibre's:
#
#   calibre-debug -e tools/oracle/send_books.py -- <library> <kobo> <product> <ops.json>
#
# <kobo> is a folder made to look like a mounted Kobo, with .kobo/version and
# .kobo/KoboReader.sqlite; <product> its USB product id. The connection is
# DeviceManager's open and post_open_callback(), then the GUI's info_read()
# and metadata_downloaded(): driveinfo.calibre, the books KoboTouch lists and
# set_books_in_library(). The ops file is an array of:
#   { "send": [1, 2] }            sync_to_device(), then books_uploaded()
#   { "remove": ["Vian, Boris/…"] } remove_paths(), then books_deleted()
#   { "matches": "out.json" }     the books on the Kobo, as Calibre matches
#                                 them with the library's: [[lpath, in_library,
#                                 application_id], …]
# The GUI's own methods run on a stand-in for its window, the device jobs
# there and then.

import contextlib
import json
import os
import sys
import types

from calibre.gui2 import Application

app = Application([], headless=True)

import calibre.gui2.device as gdevice  # noqa: E402
from calibre.devices.kobo.driver import KOBOTOUCH  # noqa: E402
from calibre.gui2.device import DeviceManager, DeviceMixin  # noqa: E402
from calibre.gui2.library.models import BooksModel  # noqa: E402
from calibre.library import db as open_library  # noqa: E402

library, kobo, product, ops_path = sys.argv[-4:]
with open(ops_path, encoding='utf-8') as f:
    ops = json.load(f)

db = open_library(library)


class NoPageCounts:
    # Calibre counts pages in the background, which sending has nothing to
    # do with.
    def queue_scan(self, book_id=0):
        pass

    def shutdown(self):
        pass

    def wait_for_worker_shutdown(self, timeout=None):
        pass


db.new_api.maintain_page_counts.shutdown()
db.new_api.maintain_page_counts = NoPageCounts()

dev = KOBOTOUCH(None)
dev.startup()
dev.reset(detected_device=(0x2237, int(product, 0), 0x0110, 'Kobo', 'eReader', ''), report_progress=None)
dev._main_prefix = os.path.abspath(kobo) + os.sep
dev.current_library_uuid = db.library_id
dev.post_open_callback()


class Job:
    def __init__(self, result=None):
        self.result, self.exception, self.failed = result, None, False


class Manager:
    # DeviceManager's jobs, run there and then.
    def __init__(self, device):
        self.device = self.connected_device = device
        self.is_device_connected = True

    def upload_books(self, done, files, names, on_card=None, titles=None, metadata=None, plugboards=None,
                     add_as_step_to_job=None):
        return Job(DeviceManager._upload_books(self, files, names, on_card=on_card, metadata=metadata,
                                               plugboards=plugboards))

    def add_books_to_metadata(self, locations, metadata, booklists):
        DeviceManager.add_books_to_metadata(self, locations, metadata, booklists)

    def sync_booklists(self, done, booklists, plugboards, add_as_step_to_job=None):
        if hasattr(self.connected_device, 'set_plugboards'):
            self.connected_device.set_plugboards(plugboards, gdevice.find_plugboard)
        DeviceManager._sync_booklists(self, booklists)

    def delete_books(self, done, paths, add_as_step_to_job=None):
        DeviceManager._delete_books(self, paths)
        return Job()

    def remove_books_from_metadata(self, paths, booklists):
        DeviceManager.remove_books_from_metadata(self, paths, booklists)


class Model:
    def __init__(self, db):
        self.db = db

    get_preferred_formats_from_ids = BooksModel.get_preferred_formats_from_ids
    metadata_for = BooksModel.metadata_for

    def refresh_ondevice(self):
        pass

    def resort(self, reset=False):
        pass

    def research(self):
        pass

    def deletion_done(self, job, failed):
        pass

    def paths_deleted(self, paths):
        pass


class Gui(DeviceMixin):
    def __init__(self):
        self.current_db = db
        self.device_manager = Manager(dev)
        view = types.SimpleNamespace(_model=Model(db), preserve_state=contextlib.nullcontext,
                                     currentIndex=lambda: types.SimpleNamespace(row=lambda: -1, isValid=lambda: True),
                                     refresh_book_details=lambda: None)
        self.library_view = self.memory_view = self.card_a_view = self.card_b_view = view
        self.status_bar = types.SimpleNamespace(show_message=lambda *a, **k: None)
        self.iactions = {'Remove Books': types.SimpleNamespace(delete_memory={})}
        self.upload_memory = {}
        self.lists = None
        self.uploads = []

    def current_view(self):
        return self.library_view

    def booklists(self):
        return self.lists

    def auto_convert_question(self, msg, autos):
        return False

    def upload_books(self, files, names, metadata, on_card=None, memory=None):
        DeviceMixin.upload_books(self, files, names, metadata, on_card=on_card, memory=memory)
        self.uploads.append(list(self.upload_memory)[-1])


gdevice.warning_dialog = lambda *a, **k: types.SimpleNamespace(exec=lambda: None)
gui = Gui()

DeviceManager._get_device_information(gui.device_manager)
DeviceManager._set_library_information(gui.device_manager, os.path.basename(db.library_path), db.library_id,
                                       db.field_metadata)
gui.set_default_thumbnail(dev.THUMBNAIL_HEIGHT)
gui.lists = list(DeviceManager._books(gui.device_manager))
gui.set_books_in_library(gui.lists, reset=True)

for op in ops:
    if 'send' in op:
        gui.uploads = []
        gui.sync_to_device(None, False, send_ids=op['send'])
        for job in gui.uploads:
            gui.books_uploaded(job)
    elif 'remove' in op:
        paths = [dev._main_prefix + lpath for lpath in op['remove']]
        job = gui.remove_paths(paths)
        gui.iactions['Remove Books'].delete_memory[job] = (paths, Model(db))
        gui.books_deleted(job)
    elif 'matches' in op:
        with open(op['matches'], 'w', encoding='utf-8') as f:
            json.dump(sorted([book.lpath, book.in_library, book.application_id] for book in gui.lists[0]), f, indent=1)
db.close()
