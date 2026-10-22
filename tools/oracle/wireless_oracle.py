# Writes assets/wireless/calibre.json: what Calibre's wireless device server
# tells the reader apps that connect to it and that Cousoir's tells them
# alike: the extensions Calibre knows books by, in its order; the type of a
# book's file by its extension, as a book's metadata carries it; and the
# description of a library's standard fields, as a library without custom
# columns has it once open, their names in English, which Cousoir
# translates as Calibre does.
#
#   CALIBRE_OVERRIDE_LANG=en calibre-debug -e tools/oracle/wireless_oracle.py -- <source folder>

import json
import os
import shutil
import sys
import tempfile

from calibre.constants import numeric_version
from calibre.ebooks import BOOK_EXTENSIONS
from calibre.library import db as open_db
from calibre.devices.mime import mime_type_ext
from calibre.utils.config import tweaks

source = sys.argv[-1]
out = os.path.join(source, 'assets', 'wireless', 'calibre.json')
os.makedirs(os.path.dirname(out), exist_ok=True)
work = tempfile.mkdtemp()
library = os.path.join(work, 'library')
shutil.copytree(os.path.join(source, 'tests', 'data', 'library'), library)
db = open_db(library)


def mime(ext):
    # Book.__init__(): mime_type_ext() of the lpath's extension.
    return mime_type_ext(ext)


tables = {
    'calibreVersion': list(numeric_version),
    'validExtensions': list(BOOK_EXTENSIONS),
    'mimeTypes': {ext: mime(ext) for ext in sorted(set(BOOK_EXTENSIONS) | {'kepub'})},
    'pubdateFormat': tweaks['gui_pubdate_display_format'],
    'timestampFormat': tweaks['gui_timestamp_display_format'],
    'lastModifiedFormat': tweaks['gui_last_modified_display_format'],
    'fieldMetadata': db.field_metadata.all_metadata(),
}
db.close()
shutil.rmtree(work)
with open(out, 'w', encoding='utf-8') as f:
    json.dump(tables, f, indent=1, ensure_ascii=False)
    f.write('\n')
