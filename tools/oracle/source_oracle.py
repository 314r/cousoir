# Writes the words Calibre's metadata sources search a book by, and the
# titles they compare results by, for a set of titles and authors, as the
# fixture metadata_web_test.cpp compares Cousoir's Open Library source
# against. Run it through Calibre's own interpreter:
#
#   CALIBRE_OVERRIDE_LANG=en calibre-debug -e tools/oracle/source_oracle.py -- tests/data/oracle/source.json

import json
import sys

from calibre.constants import __version__
from calibre.ebooks.metadata.sources.base import Source, cleanup_title

TITLES = [
    'The Black Company', 'Shadows Linger', "L'Écume des jours", 'Le Petit Prince', 'A Tale of Two Cities',
    'The Lord of the Rings (Omnibus)', 'Dune (1965)', 'Harry Potter (Paperback)', 'Foundation [Mass Market]',
    'Neuromancer (20th Anniversary Edition)', 'The Hobbit (ed. 2)', '1,000,000 Words', 'Good Omens - The Novel',
    'Spider-Man', 'Title: A Subtitle', '"Quoted"', "'Single'", 'And Then There Were None', 'A & B', 'the',
    'War and Peace', '  Spaced   Out  ', 'Les Misérables, tome 1', 'C++ Primer', 'Un amour de Swann',
    'Flatland: A Romance of Many Dimensions', 'Title 《Chinese》', '',
]
AUTHORS = [
    ['Glen Cook'], ['Boris Vian'], ['Antoine de Saint-Exupéry'], ['Cook, Glen'], ['J. R. R. Tolkien'],
    ['Ludwig van Beethoven'], ['Wernher von Braun'], ["Flann O'Brien"], ['Terry Pratchett', 'Neil Gaiman'],
    ['Unknown'], ['Jean-Paul Sartre'], ['Al Ng'], ['Edwin A. Abbott'], ['[Anonymous]'], [],
]

out = {
    'calibre_version': __version__,
    'title_tokens': [[t, list(Source.get_title_tokens(None, t))] for t in TITLES],
    'author_tokens': [[a, list(Source.get_author_tokens(None, a, only_first_author=True))] for a in AUTHORS],
    'cleanup_title': [[t, cleanup_title(t)] for t in TITLES],
}
with open(sys.argv[-1], 'w', encoding='utf-8') as f:
    json.dump(out, f, ensure_ascii=False, indent=1)
