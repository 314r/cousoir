# Writes the tokens Calibre's own FTS5 tokenizer (src/calibre/db/
# sqlite_extension.cpp), which Calibre registers as "unicode61", "calibre"
# and "porter" on its connections, makes of a set of texts, in two interface
# languages, as calibre_fts_test.cpp compares Cousoir's port against:
#
#   calibre-debug -e tools/oracle/fts_oracle.py -- tests/data/oracle/fts.json

import json
import sys

import apsw

from calibre.constants import __version__, plugins
from calibre_extensions.sqlite_extension import set_ui_language

TEXTS = [
    "Chick, Alise, Chloé et Colin passent leur temps à dire des choses rigolotes",
    "L'Écume des jours, l’amour fou",
    "The Black Company's annals: running, ran, runs; 3 TIMES",
    "Straße, ÉCOLE, naïve café, œuvre",
    "東京は日本の首都です。北京大学",
    "สวัสดีครับ ภาษาไทย",
    "e-mail don't co-operate — dash–en 1,000.50 $5 ©2024 ★",
    "Ελληνικά κείμενα, ΚΕΦΑΛΑΙΑ",
    "Mixed 日本語 and English words",
    "",
]
TABLES = {
    'plain': "unicode61 remove_diacritics 2",
    'stemmed': "porter unicode61 remove_diacritics 2",
    'calibre': "calibre remove_diacritics 2",
}

out = {'calibre_version': __version__, 'languages': {}}
for language in ('en', 'fr'):
    set_ui_language(language)
    conn = apsw.Connection(':memory:')
    plugins.load_apsw_extension(conn, 'sqlite_extension')
    found = {}
    for name, tokenize in TABLES.items():
        conn.execute(f"CREATE VIRTUAL TABLE t_{name} USING fts5(text, tokenize = '{tokenize}')")
        conn.execute(f"CREATE VIRTUAL TABLE v_{name} USING fts5vocab(t_{name}, 'instance')")
        for i, text in enumerate(TEXTS):
            conn.execute(f'INSERT INTO t_{name}(rowid, text) VALUES (?, ?)', (i + 1, text))
        found[name] = [
            [[term, offset] for term, offset in conn.execute(
                f'SELECT term, offset FROM v_{name} WHERE doc=? ORDER BY offset, term', (i + 1,))]
            for i in range(len(TEXTS))]
    out['languages'][language] = found
out['texts'] = TEXTS
out['tables'] = TABLES
with open(sys.argv[-1], 'w', encoding='utf-8') as f:
    json.dump(out, f, ensure_ascii=False, indent=1)
