# Runs searches the way calibredb search runs them, for the golden test that
# compares Cousoir's search with Calibre's:
#
#   calibre-debug -e tools/oracle/search_books.py -- <library> <queries.txt> <out.json>
#
# Each line of the queries file is a search; a line "@vl NAME" makes the
# virtual library NAME the search restriction of the lines after it, as the
# Calibre window does, and "@vl" alone lifts it. Blank lines and lines
# starting with "# " are left out: "#genre:sf" is a search. The output lists, for each search, the
# restriction, the ids found in order, or whether the search failed, or
# Calibre did.

import json
import sys

from calibre.library import db as open_library
from calibre.utils.search_query_parser import ParseException

library, queries_path, out_path = sys.argv[-3:]
cache = open_library(library).new_api
virtual_libraries = cache.pref('virtual_libraries', {})

results = []
restriction = ''
with open(queries_path, encoding='utf-8') as f:
    for line in f:
        query = line.rstrip('\n')
        if not query.strip() or query.startswith('# '):
            continue
        if query.startswith('@vl'):
            name = query[3:].strip()
            restriction = virtual_libraries.get(name, '') if name else ''
            continue
        entry = {'query': query, 'restriction': restriction}
        try:
            entry['ids'] = sorted(cache.search(query, restriction))
        except ParseException:
            entry['error'] = True
        except Exception as e:
            # Calibre's own bugs, series_sort:true for one: nothing to
            # compare with.
            entry['crash'] = '%s: %s' % (type(e).__name__, e)
        results.append(entry)

with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(results, f, ensure_ascii=False, indent=0)
    f.write('\n')
