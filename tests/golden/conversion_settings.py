#!/usr/bin/env python3
# Gives books of a copy of the fixture library the settings earlier
# conversions saved, as Calibre stores them in conversion_options:
#
#   conversion_settings.py <library>
#
# - book 1: a Python literal, as Calibre wrote them before JSON, which
#   Calibre 9.16 no longer reads;
# - book 2: AZW3 as the format to convert from, so that converting it to
#   AZW3 keeps its file as ORIGINAL_AZW3;
# - book 3: another output profile and smartened punctuation.

import sqlite3
import struct
import sys


def pickled(raw):
    # pickle_binary_string() of calibre/utils/__init__.py.
    return b'\x80\x02T' + struct.pack('<i', len(raw)) + raw + b'.'


SETTINGS = {
    1: b"{\n\t'gui_preferred_input_format' : 'azw3',\n\t'output_profile' : 'kindle_pw',\n"
       b"\t'change_justification' : u'justify',\n}",
    2: b'json:{\n  "gui_preferred_input_format": "azw3"\n}',
    3: b'json:{\n  "output_profile": "kobo",\n  "smarten_punctuation": true,\n'
       b'  "gui_preferred_input_format": "epub"\n}',
}

con = sqlite3.connect(sys.argv[1] + '/metadata.db')
for book, raw in SETTINGS.items():
    con.execute("INSERT INTO conversion_options(book, format, data) VALUES (?, 'PIPE', ?)", (book, pickled(raw)))
con.commit()
con.close()
