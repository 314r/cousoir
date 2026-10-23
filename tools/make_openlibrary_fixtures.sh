#!/usr/bin/env bash
# Writes tests/data/openlibrary: Open Library's answers to the searches
# Cousoir makes for two books of the fixture library, and the works they
# found, which the tests serve from a fake server (tests/fake_open_library.h).
# Run by hand; the tests never reach the network.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/data/openlibrary"
mkdir -p "$out/works"
agent="Cousoir (https://github.com/314r)"
fields="key,title,author_name,first_publish_year,cover_i,subject,editions,editions.key,editions.title,editions.language,editions.publisher,editions.publish_date,editions.isbn,editions.cover_i"
fetch() { curl -sfL -A "$agent" "$1" -o "$2"; }
fetch "https://openlibrary.org/search.json?isbn=9782253140870&fields=$fields&limit=10" "$out/search-isbn.json"
fetch "https://openlibrary.org/search.json?title=Shadows+Linger&author=Glen+Cook&fields=$fields&limit=10&lang=en" "$out/search-title.json"
for work in $(python3 -c '
import json, sys
for name in sys.argv[1:]:
    for doc in json.load(open(name))["docs"][:5]:
        print(doc["key"].split("/")[-1])' "$out/search-isbn.json" "$out/search-title.json" | sort -u); do
    fetch "https://openlibrary.org/works/$work.json" "$out/works/$work.json"
done
