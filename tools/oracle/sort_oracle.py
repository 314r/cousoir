# Prints the order Calibre's book list gives a library's books for each sort
# of a JSON file, for the golden test that compares Cousoir's sorting:
#
#   calibre-debug -e tools/oracle/sort_oracle.py -- <library> <sorts.json> <output.json>
#
# A sort is a list of [field, ascending] pairs, the most significant first,
# as Calibre's list keeps its sort history: [["series", true], ["title",
# true]] sorts by series, then title. Each prints Cache.multisort() of
# src/calibre/db/cache.py on every book, taken in the order of their ids, as
# the list's view sorts them again by its sort history (db/view.py).

import json
import sys

from calibre.library import db as open_library

library, sorts_path, out = sys.argv[-3:]
with open(sorts_path, encoding='utf-8') as f:
    sorts = json.load(f)

legacy = open_library(library)
cache = legacy.new_api
# The books by id, as the list's view (db/view.py) has them before a sort.
book_ids = sorted(cache.all_book_ids())
results = []
for sort in sorts:
    try:
        ids = cache.multisort([(field, bool(ascending)) for field, ascending in sort], ids_to_sort=book_ids)
        results.append({'sort': sort, 'ids': list(ids)})
    except Exception as e:
        results.append({'sort': sort, 'error': f'{type(e).__name__}: {e}'})
with open(out, 'w', encoding='utf-8') as f:
    json.dump(results, f, ensure_ascii=False, indent=1)
