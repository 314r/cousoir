# Writes the SQL that makes the metadata.db of a new, empty library as
# Calibre makes one, which Cousoir runs to create a library without
# Calibre (new_library.cpp):
#
#   calibre-debug -e tools/oracle/new_library_sql.py -- assets/calibre/new_library.sql
#
# Every table, index, trigger and view in the order Calibre made them, the
# virtual tables by their own statements, which make their shadow tables;
# the rows of the tables that have some but those shadow tables; then the
# database's application id and schema version. The library's uuid is
# Calibre's of the day: Cousoir gives each library its own.

import os
import sys
import tempfile

import apsw

from calibre.constants import __version__
from calibre.library import db as open_library

out = sys.argv[-1]


def literal(value):
    if value is None:
        return 'NULL'
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, bytes):
        return "X'" + value.hex() + "'"
    return "'" + str(value).replace("'", "''") + "'"


with tempfile.TemporaryDirectory() as folder:
    open_library(folder).close()
    conn = apsw.Connection(os.path.join(folder, 'metadata.db'))
    cursor = conn.cursor()
    objects = list(cursor.execute('SELECT type, name, sql FROM sqlite_master ORDER BY rowid'))
    virtual = [name for kind, name, sql in objects if sql and sql.startswith('CREATE VIRTUAL TABLE')]
    # FTS5's own tables of each virtual table.
    shadow = {v + suffix for v in virtual for suffix in ('_data', '_idx', '_content', '_docsize', '_config')}
    lines = [f'-- The metadata.db of a new library, as Calibre {__version__} makes it.',
             '-- Written by tools/oracle/new_library_sql.py; do not edit.']
    for kind, name, sql in objects:
        if sql is None or name in shadow or name.startswith('sqlite_'):
            continue
        lines.append(sql + ';')
    for kind, name, sql in objects:
        if kind != 'table' or sql is None or name in shadow or name.startswith('sqlite_') or name in virtual:
            continue
        columns = [row[1] for row in cursor.execute(f'PRAGMA table_info("{name}")')]
        for row in cursor.execute(f'SELECT * FROM "{name}" ORDER BY rowid'):
            values = ', '.join(literal(value) for value in row)
            lines.append(f'INSERT INTO "{name}" ({", ".join(columns)}) VALUES ({values});')
    application_id = next(cursor.execute('PRAGMA application_id'))[0]
    user_version = next(cursor.execute('PRAGMA user_version'))[0]
    lines.append(f'PRAGMA application_id={application_id};')
    lines.append(f'PRAGMA user_version={user_version};')
    conn.close()

os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with open(out, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
