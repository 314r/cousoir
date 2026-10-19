# Writes the title and author sorts Calibre computes for a set of names, and
# its other text helpers' results, as the fixture calibre_text_test.cpp
# compares the port against. Run it through
# Calibre's own interpreter:
#
#   CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/text_oracle.py -- tests/data/oracle/text.json
#
# Calibre's default tweaks apply; a tweaks.json in the Calibre configuration
# folder would change the expected values, so the script refuses to run with
# one.

import json
import os
import sys

from calibre.constants import __version__, config_dir
from calibre import replace_entities
from calibre.ebooks.metadata import author_to_author_sort, authors_to_string, check_doi, check_isbn, string_to_authors, title_sort
from calibre.utils.cleantext import clean_ascii_chars, clean_xml_chars
from calibre.utils.localization import canonicalize_lang, get_lang

if os.path.exists(os.path.join(config_dir, 'tweaks.json')):
    raise SystemExit('Move %s away first: the fixture records the default tweaks.' % os.path.join(config_dir, 'tweaks.json'))

TITLES = [
    'The Black Company', 'A Tale of Two Cities', 'An Apple a Day', 'Thermodynamics', 'Theo', 'The', 'THE CAPS',
    'the lower case', '  The  Double  Spaces  ', 'Le Petit Prince', 'La Peste', "L'Étranger", 'L’Écume des jours',
    'L´Accent', 'Les Misérables', 'Lest We Forget', 'Lettres persanes', 'Leçons', 'Un amour de Swann', 'Une vie',
    'Des souris et des hommes', 'De la démocratie en Amérique', 'De La Terre à la Lune', 'De Gaulle', "D'Artagnan",
    'D’Artagnan', 'Der Prozess', 'Die Verwandlung', 'Das Boot', 'El Aleph', 'Il nome della rosa', "Dell'amore",
    "'s Avonds", "'t Hooft", 'Het Achterhuis', '"Quoted Title"', '"The Quoted"', '“The Curly”', '« Le Titre »',
    '‘Single’', "'Apostrophes'", 'Le Insécable', 'Le\tTab', 'Les', 'Un', '', '   ', 'Glen Cook',
    'American Tabloid (Rivages/Noir) (French Edition)', 'Les Coqs cubains chantent à minuit', 'Une Étoile',
    '"L\'Étranger"', 'O Alquimista', 'Os Lusíadas', 'Ett år', 'Bir Gün', 'Egy nap', 'Az ember',
]
LANGUAGES = [None, 'eng', 'fra', 'deu', 'spa', 'ita', 'nld', 'por', 'swe', 'tur', 'hun', 'pol', 'zzz', 'fr', 'en']

AUTHORS = [
    'Glen Cook', 'John Schember', 'Boris Vian', 'Antoine de Saint-Exupéry', 'Jean-Paul Sartre', 'J. R. R. Tolkien',
    'Martin Luther King Jr.', 'Martin Luther King Jr', 'Dr. Martin Luther King, Jr.', 'Ursula K. Le Guin',
    'Madonna', 'Microsoft Corporation', 'Clarke, Arthur C.', 'Browne, Harry, 1933-2006', 'Edward Jentz III',
    'Prof. Dr. Hans Müller', 'Mr. Smith', 'Smith Jr', 'Jr', 'Mrs', 'Mr Dr', 'Mr. Dr.', 'Gabriel García Márquez',
    'Ludwig van Beethoven', 'Vincent van Gogh', 'van Gogh', 'Le Guin', '  John   Doe  ', 'John (Jack) Doe',
    '[Editor] John Doe', 'John Doe (ed.)', '(Anonymous)', 'Hugh Amano', 'Card Orson Scott', 'BLOCH, Marc',
    'Celine', 'Unknown', 'Inconnu(e)', '', 'The Pierre Team', 'Acme Software Inc.', 'Anne-Marie de la Tour',
    'Nassim Nicholas Taleb', 'Alba De Cespedes', 'King III Jr.', 'Ph.D John', 'John Smith Ph.D', 'John Smith PhD.',
    'Société Nationale', 'Jean Dupont', 'José Saramago', 'Lao Tzu', 'Charles Matthews, Seong-June Kim',
]
METHODS = ['comma', 'nocomma', 'invert', 'copy']
SQL_AUTHORS = ['Doe| John', 'Glen Cook', '', 'Doe|John|Jr']
RAW_LANGUAGES = ['fr', 'FR', 'fr_FR', 'fr-FR', 'fr_FR.UTF-8', 'sr@latin', 'fra', 'eng', 'en', 'de', 'pt_BR', 'zh_CN',
                 'nl', 'xx', '', 'zzz']

AUTHOR_STRINGS = [
    'Glen Cook', 'A & B', 'A and B', 'A AND B', 'A, and B', 'A with B', 'Andrew Anderson', 'Withers Bob', 'A&&B & C',
    ' A  &  B ', '&', '', 'A & ', 'Smith, John and Doe, Jane', 'Rock and Roll band', 'A\u00a0and\u00a0B', 'A andB',
]
AUTHOR_LISTS = [['A', 'B'], ['A&B', 'C'], ['Smith, John'], []]
ISBNS = [
    '978-2-07-036024-5', '9782070360246', '2-07-036024-6', '207036024X', '0-306-40615-2', '0306406152',
    'ISBN 978-0-306-40615-7', '978 0 306 40615 7', '9780306406158', '1111111111', '0000000000000', '',
    'urn:isbn:9780306406157', '0-8044-2957-x', 'abc', '978-0-306-40615-7 and more 1',
]
DOIS = ['10.1000/182', 'doi:10.1234/rel.2019', 'https://doi.org/10.1038/nphys1170 x', '10.12/abc', '', 'none']
ENTITIES = [
    'Tom &amp; Jerry', '&lt;b&gt;', '&eacute;t&eacute;', '&#233;', '&#xE9;', '&#XE9;', '&#0;', '&#;', '&#x;',
    '&unknown;', '&amp', '& amp;', '&&amp;', '&a&amp;', 'AT&T', '&nbsp;x', '&#128512;', '&' + 'a' * 70 + ';',
    '&AMP;', '&squot;', '&hellips;', '&#12a;', 'Fish &chips; &amp; peas', '',
]
DIRTY = ['plain', 'tab\there', 'bell\x07x', 'del\x7fx', 'nel\x85x', 'c1\x80\x9fx', 'ok\ufffdx', 'nonchar\ufffex',
         'arabic\ufdd0x', 'emoji\U0001F600x', 'cr\r\nlf', 'sep\x1cx']

title_sorts = []
for lang in LANGUAGES:
    for title in TITLES:
        title_sorts.append({'title': title, 'lang': lang, 'expected': title_sort(title, lang=lang)})
for title in TITLES[:12]:
    title_sorts.append({'title': title, 'lang': 'eng', 'order': 'strictly_alphabetic',
                        'expected': title_sort(title, order='strictly_alphabetic', lang='eng')})

author_sorts = []
for method in METHODS:
    for author in AUTHORS:
        author_sorts.append({'author': author, 'method': method, 'expected': author_to_author_sort(author, method=method)})
for author in AUTHORS:
    author_sorts.append({'author': author, 'method': 'comma', 'surname_prefixes': True,
                         'expected': author_to_author_sort(author, method='comma', use_surname_prefixes=True)})

sql_author_sorts = [{'author': a, 'expected': author_to_author_sort(a.replace('|', ',')) if a else ''}
                    for a in SQL_AUTHORS]

languages = [{'raw': raw, 'expected': canonicalize_lang(raw)} for raw in RAW_LANGUAGES]

authors = [{'text': t, 'expected': string_to_authors(t)} for t in AUTHOR_STRINGS]
author_lists = [{'authors': a, 'expected': authors_to_string(a)} for a in AUTHOR_LISTS]
isbns = [{'text': t, 'expected': check_isbn(t) or '', 'simple': check_isbn(t, simple_sanitize=True) or ''} for t in ISBNS]
dois = [{'text': t, 'expected': check_doi(t) or ''} for t in DOIS]
entities = [{'text': t, 'expected': replace_entities(t)} for t in ENTITIES]
cleaned = [{'text': t, 'ascii': clean_ascii_chars(t), 'xml': clean_xml_chars(t)} for t in DIRTY]

with open(sys.argv[-1], 'w', encoding='utf-8') as f:
    json.dump({
        'calibre_version': __version__,
        'interface_language': get_lang(),
        'title_sort': title_sorts,
        'author_sort': author_sorts,
        'sql_author_sort': sql_author_sorts,
        'canonicalize_lang': languages,
        'string_to_authors': authors,
        'authors_to_string': author_lists,
        'check_isbn': isbns,
        'check_doi': dois,
        'replace_entities': entities,
        'clean_chars': cleaned,
    }, f, ensure_ascii=False, indent=1)
    f.write('\n')
