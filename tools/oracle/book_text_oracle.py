# Records the text Calibre's full text index keeps for book files of the
# fixtures, as its indexing workers extract it (calibre.db.fts.text), which
# book_text_index_test.cpp compares Cousoir's extraction against:
#
#   calibre-debug -e tools/oracle/book_text_oracle.py -- <tests/data> <out.json>
#
# Each file is copied first: Calibre's readers may leave files beside it.

import json
import os
import shutil
import sys
import tempfile

from calibre.constants import __version__
from calibre.db.fts.text import extract_text

FILES = [
    'library/Boris Vian/L\'Ecume des jours (1)/L\'Ecume des jours - Boris Vian.epub',
    'library/Glen Cook/The Black Company (2)/The Black Company - Glen Cook.epub',
    'library/Glen Cook/The Black Company (2)/The Black Company - Glen Cook.azw3',
    'library/Jean-Pierre Dupont/Manuel technique (5)/Manuel technique - Jean-Pierre Dupont.pdf',
    'import/Black Company 2.epub',
    'import/Custom 2.epub',
    'import/Le Vicomte - Inconnu.epub',
    'import/manga.epub',
    'import/The Lost Title - Anne Onyme & Bob Builder.epub',
    'import/calibre.pdf',
    'import/relativity.pdf',
    'kepub/rich.epub',
    'mobi/bare.mobi',
    'mobi/vicomte.azw3',
    'mobi/vicomte.mobi',
    'reader/cfi.epub',
    'reader/fixed.epub',
    'reader/notes.epub',
    'reader/voyage.epub',
    'reader/voyage.azw3',
    'reader/voyage.mobi',
    'reader/voyage-old.mobi',
]

root, out = sys.argv[-2:]
texts = {}
with tempfile.TemporaryDirectory() as folder:
    for index, name in enumerate(FILES):
        copy = os.path.join(folder, f'{index}.{name.rpartition(".")[-1]}')
        shutil.copyfile(os.path.join(root, name), copy)
        texts[name] = extract_text(copy)
with open(out, 'w', encoding='utf-8') as f:
    json.dump({'calibre': __version__, 'texts': texts}, f, indent=1, ensure_ascii=False)
    f.write('\n')
