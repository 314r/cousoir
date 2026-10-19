# Writes the title and author sorts Calibre computes for a set of names, as
# the fixture calibre_text_test.cpp compares the port against. Run it through
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
from calibre.ebooks.metadata import author_to_author_sort, title_sort
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

with open(sys.argv[-1], 'w', encoding='utf-8') as f:
    json.dump({
        'calibre_version': __version__,
        'interface_language': get_lang(),
        'title_sort': title_sorts,
        'author_sort': author_sorts,
        'sql_author_sort': sql_author_sorts,
        'canonicalize_lang': languages,
    }, f, ensure_ascii=False, indent=1)
    f.write('\n')
