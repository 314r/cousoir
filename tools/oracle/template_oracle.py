# Prints what Calibre's templates make of each book of a library, in the
# shape `cousoir-cli template` prints Cousoir's:
#
#   calibre-debug -e tools/oracle/template_oracle.py -- <library> <templates.json> <output.json>
#
# Each template of the JSON array is evaluated on each book as Calibre
# evaluates a composite column (CompositeField._render_composite_with_cache()
# of src/calibre/db/fields.py): SafeFormat.safe_format() on the book's
# ProxyMetadata, by one formatter. A template that fails prints its error,
# {"error": message}, as Calibre writes it after its "TEMPLATE ERROR".

import json
import sys

from calibre.ebooks.metadata.book.formatter import SafeFormat
from calibre.library import db as open_library

library, templates_path, out = sys.argv[-3:]
with open(templates_path, encoding='utf-8') as f:
    templates = json.load(f)

# The library kept open: a template's with statement reads other books.
legacy = open_library(library)
cache = legacy.new_api
book_ids = sorted(cache.all_book_ids())
formatter = SafeFormat()
sentinel = '\x00error\x00'
results = []
for template in templates:
    row = {}
    for book_id in book_ids:
        mi = cache.get_proxy_metadata(book_id)
        value = formatter.safe_format(template, mi, sentinel, mi)
        row[str(book_id)] = {'error': value[len(sentinel) + 1:]} if value.startswith(sentinel) else value
    results.append({'template': template, 'books': row})
with open(out, 'w', encoding='utf-8') as f:
    json.dump(results, f, ensure_ascii=False, indent=1)
