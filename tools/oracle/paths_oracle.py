# Writes the file and folder names Calibre gives books, as the fixture
# calibre_paths_test.cpp compares the port against:
#
#   CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/paths_oracle.py -- tests/data/oracle/paths.json

import json
import sys

from calibre.constants import __version__
from calibre.db.backend import DB
from calibre.utils.filenames import ascii_filename
from calibre.utils.localization import get_lang

TEXTS = [
    "L'Écume des jours", 'Boris Vian', 'The Black Company', 'Glen Cook', 'Rivages/Noir: (French Edition)',
    'Qu\'est-ce que c\'est ?', 'a*b<c>d|e"f', 'Antoine de Saint-Exupéry', 'Œuvres complètes', 'Straße',
    '北京', '東京物語', 'Москва', 'Ελληνικά', 'עברית', 'emoji 📚 book', 'tab\there', 'line\nbreak',
    '  leading and trailing  ', '...', '.hidden', 'ends with dot.', 'ends with space ', 'a..b', 'CON', 'con',
    'Lpt1', '', '   ', 'Unknown', 'Inconnu(e)', '[brackets] and +plus+', 'Ł ø đ ß æ', 'ﬁ ligature ½',
    'No\u00a0break', 'Chop Wood, Carry Water: The Yoga of Work',
    'A very long title that goes on and on well past the limit Calibre puts on folder names in its libraries',
    'Ünïcödé ÀÉÎÕÜ àéîõü çÇ ñÑ', 'trailing dots...', 'C:\\path\\like', 'x' * 120,
]

names = [{'text': text, 'expected': ascii_filename(text)} for text in TEXTS]
db = DB.__new__(DB)
paths = []
for book_id in (1, 42, 137, 123456):
    for title, author in ((TEXTS[0], TEXTS[1]), (TEXTS[4], TEXTS[3]), (TEXTS[10], TEXTS[11]), ('', ''),
                          ('   ', 'CON'), (TEXTS[-3], TEXTS[-2]), (TEXTS[-1], TEXTS[-1]), (TEXTS[20], TEXTS[21]),
                          (TEXTS[22], TEXTS[23]), (TEXTS[35], 'Andrew J. Taggart')):
        for extlen in (4, 5, 10, 15, 30):
            paths.append({
                'id': book_id, 'title': title, 'author': author, 'extlen': extlen,
                'path': db.construct_path_name(book_id, title, author),
                'file': db.construct_file_name(book_id, title, author, extlen),
            })

with open(sys.argv[-1], 'w', encoding='utf-8') as f:
    json.dump({'calibre_version': __version__, 'interface_language': get_lang(), 'ascii_filename': names,
               'paths': paths}, f, ensure_ascii=False, indent=1)
    f.write('\n')
