# Connects a Kobo to Calibre's GUI and sends books to it or removes books
# from it, as the GUI does, for the golden test that compares Cousoir's
# sending with Calibre's:
#
#   calibre-debug -e tools/oracle/send_books.py -- <library> <device> <product> <ops.json>
#
# <device> is a folder made to look like a mounted Kobo, with .kobo/version
# and .kobo/KoboReader.sqlite, "mtpdir:FOLDER", a folder that stands for a
# Kindle reached over MTP, or "wireless:PORT", Calibre's wireless device
# server listening on PORT, to which a reader app connects
# (tests/wireless/fake_koreader.py); <product> its USB product id. A Kobo's connection
# is DeviceManager's open and post_open_callback(), a Kindle's the MTP
# driver's open() with libmtp's calls done on the folder; then the GUI's
# info_read() and metadata_downloaded(): driveinfo.calibre, the books the
# driver lists and set_books_in_library(). The ops file is an array of:
#   { "send": [1, 2] }            sync_to_device(), then books_uploaded()
#   { "remove": ["Vian, Boris/…"] } remove_paths(), then books_deleted(), the
#                                 books by their path on the device
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

library, device, product, ops_path = sys.argv[-4:]
with open(ops_path, encoding='utf-8') as f:
    ops = json.load(f)
mtp = device.startswith('mtpdir:')
wireless = device.startswith('wireless:')

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



def kindle(root):
    # Calibre's MTP driver with libmtp's calls done on a folder, and what
    # its open() does but for libmtp's part. The Kindle says of itself what
    # cousoir-cli's stand-in for one says. FolderDevice's methods follow those
    # of src/calibre/devices/mtp/unix/driver.py, and the lines after it open()
    # of src/calibre/devices/mtp/driver.py, Copyright (C) Kovid Goyal, GPL v3.
    import itertools

    from calibre.devices.mtp.driver import DEFAULT_THUMBNAIL_HEIGHT, MTP_DEVICE
    from calibre.devices.mtp.filesystem_cache import FilesystemCache
    from calibre.utils.date import isoformat, utcnow

    ids = itertools.count(1)
    storage_id = 0x10001

    def entry(parent, name, full):
        is_folder = os.path.isdir(full)
        return {'id': next(ids), 'parent_id': 0 if parent.is_storage else parent.object_id, 'storage_id': storage_id,
                'name': name, 'is_folder': is_folder, 'size': 0 if is_folder else os.path.getsize(full),
                'modified': os.path.getmtime(full)}

    class FolderDevice(MTP_DEVICE):
        def full(self, obj):
            return os.path.join(root, *obj.full_path[1:])

        @property
        def filesystem_cache(self):
            if self._filesystem_cache is None:
                stats = os.statvfs(root)
                items = []

                # get_filesystem(): every object, the folders not ignored
                # looked into.
                def walk(folder, parent_id, path):
                    for name in sorted(os.listdir(folder)):
                        full = os.path.join(folder, name)
                        is_folder = os.path.isdir(full)
                        item = {'id': next(ids), 'parent_id': parent_id, 'storage_id': storage_id, 'name': name,
                                'is_folder': is_folder, 'size': 0 if is_folder else os.path.getsize(full),
                                'modified': os.path.getmtime(full)}
                        items.append(item)
                        if is_folder and not self.is_folder_ignored(storage_id, path + (name,)):
                            walk(full, item['id'], path + (name,))

                walk(root, 0, ())
                self._filesystem_cache = FilesystemCache([{
                    'id': storage_id, 'size': stats.f_frsize * stats.f_blocks, 'is_folder': True,
                    'name': 'Internal Storage', 'can_delete': False, 'is_system': True}], items)
            return self._filesystem_cache

        @property
        def is_mtp_device_connected(self):
            return True

        def get_basic_device_information(self):
            return (self.current_friendly_name, '', '', '')

        def total_space(self, end_session=True):
            stats = os.statvfs(root)
            return (stats.f_frsize * stats.f_blocks, 0, 0)

        def free_space(self, end_session=True):
            stats = os.statvfs(root)
            return (stats.f_frsize * stats.f_bavail, 0, 0)

        def create_folder(self, parent, name):
            found = parent.folder_named(name)
            if found is not None:
                return found
            full = os.path.join(self.full(parent) if not parent.is_storage else root, name)
            os.mkdir(full)
            return parent.add_child(entry(parent, name, full))

        def put_file(self, parent, name, stream, size, callback=None, replace=True):
            existing = parent.file_named(name)
            if existing is not None:
                self.delete_file_or_folder(existing)
            full = os.path.join(self.full(parent) if not parent.is_storage else root, name)
            with open(full, 'wb') as f:
                f.write(stream.read())
            return parent.add_child(entry(parent, name, full))

        def get_mtp_file(self, f, stream=None, callback=None):
            import io

            set_name = stream is None
            stream = io.BytesIO() if stream is None else stream
            with open(self.full(f), 'rb') as src:
                stream.write(src.read())
            stream.seek(0)
            if set_name:
                stream.name = f.name
            return stream

        def delete_file_or_folder(self, obj):
            if obj.deleted:
                return
            if obj.files or obj.folders:
                raise ValueError(f'Cannot delete {obj.full_path} as it is not empty')
            if obj.is_folder:
                os.rmdir(self.full(obj))
            else:
                os.remove(self.full(obj))
            parent = obj.parent
            parent.remove_child(obj)
            return parent

    dev = FolderDevice(None)
    dev.startup()
    dev.current_library_uuid = db.library_id
    dev.location_paths = None
    dev.driveinfo = {}
    dev._main_id = storage_id
    dev._carda_id = dev._cardb_id = None
    dev.current_friendly_name = 'Kindle'
    dev.current_serial_num = 'G000000000000000'
    h = dev.prefs['history']
    h[dev.current_serial_num] = (dev.current_friendly_name, isoformat(utcnow()))
    dev.prefs['history'] = h
    detected = types.SimpleNamespace(vendor_id=0x1949, product_id=int(product, 0))
    dev.currently_connected_dev = detected
    dev.current_device_defaults, dev.current_vid, dev.current_pid = dev.device_defaults(detected, dev)
    dev.calibre_file_paths = dev.current_device_defaults.get(
        'calibre_file_paths', {'metadata': dev.METADATA_CACHE, 'driveinfo': dev.DRIVEINFO})
    dev.THUMBNAIL_HEIGHT = 500 if dev.is_kindle else DEFAULT_THUMBNAIL_HEIGHT
    dev.sync_kindle_thumbnails()
    return dev


def wireless_device(port):
    # The wireless driver started as "Start wireless device connection"
    # does, on a fixed port, then what the device manager does once a
    # reader connects: open() and the global preferences it asks for, the
    # metadata sent on connect among them.
    import time

    from calibre.devices.smart_device_app.driver import SMART_DEVICE_APP
    from calibre.utils.config import device_prefs

    dev = SMART_DEVICE_APP(None)
    dev.startup()
    dev.set_option('use_fixed_port', True)
    dev.set_option('port_number', port)
    message = dev.start_plugin()
    if message:
        raise SystemExit(message)
    deadline = time.monotonic() + 60
    while dev.detect_managed_devices([]) is None:
        if time.monotonic() > deadline:
            raise SystemExit('no reader connected')
        time.sleep(0.1)
    dev.reset(detected_device=None, report_progress=None)
    if not dev.open(None, db.library_id):
        raise SystemExit('the reader did not open')
    dev.specialize_global_preferences(device_prefs)
    return dev


if mtp:
    dev = kindle(os.path.abspath(device[7:]))
elif wireless:
    dev = wireless_device(device.partition(':')[2])
else:
    dev = KOBOTOUCH(None)
    dev.startup()
    dev.reset(detected_device=(0x2237, int(product, 0), 0x0110, 'Kobo', 'eReader', ''), report_progress=None)
    dev._main_prefix = os.path.abspath(device) + os.sep
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
        if mtp or wireless:
            removed = {lpath.lower() for lpath in op['remove']}
            paths = [book.path for book in gui.lists[0] if book.lpath.lower() in removed or book.lpath in op['remove']]
        else:
            paths = [dev._main_prefix + lpath for lpath in op['remove']]
        job = gui.remove_paths(paths)
        gui.iactions['Remove Books'].delete_memory[job] = (paths, Model(db))
        gui.books_deleted(job)
    elif 'matches' in op:
        with open(op['matches'], 'w', encoding='utf-8') as f:
            json.dump(sorted([book.lpath, book.in_library, book.application_id] for book in gui.lists[0]), f, indent=1)
db.close()
if wireless:
    dev.eject()
    dev.stop_plugin()
