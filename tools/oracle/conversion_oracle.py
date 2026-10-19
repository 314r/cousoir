# Records the OPF Calibre's convert dialog writes for each book of the
# fixture library, which a conversion reads the book's metadata from:
#
#   CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/conversion_oracle.py -- <library> <out.json>
#
# create_opf_file() of gui2/convert/metadata.py, on a copy of the library,
# which Calibre writes to as it opens it.

import json
import os
import shutil
import sys
import tempfile

from calibre.constants import __appname__, __version__
from calibre.gui2.convert.metadata import create_opf_file
from calibre.library import db as open_library

library, out = sys.argv[-2:]
with tempfile.TemporaryDirectory() as folder:
    copy = os.path.join(folder, 'library')
    shutil.copytree(library, copy)
    db = open_library(copy)
    opfs = {}
    for book_id in sorted(db.new_api.all_book_ids()):
        mi, opf_file = create_opf_file(db, book_id)
        with open(opf_file.name, encoding='utf-8') as f:
            opfs[str(book_id)] = f.read()
        os.remove(opf_file.name)
    db.close()
with open(out, 'w', encoding='utf-8') as f:
    json.dump({'producer': __appname__ + f' ({__version__}) ' + '[https://calibre-ebook.com]', 'opfs': opfs}, f,
              indent=1, ensure_ascii=False)
    f.write('\n')
