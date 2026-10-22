# Writes what Cousoir's search takes from Calibre's, which matches text with
# ICU collators rather than rules a port could follow:
#
# - calibre_search.inc: for each character Calibre's primary collator does
#   not take as its own lower case, what Cousoir folds it to: nothing for a
#   character the collator ignores, "e" for "é", "ae" for "æ", "d" for "ð".
#   Two folds: one for primary_no_punc_contains(), Calibre's default match,
#   which ignores spaces and punctuation too, and one for primary_contains(),
#   the match of a query starting with ^, which does not;
# - tests/data/oracle/search.json: queries and texts, and whether Calibre's
#   two functions find each query in each text, which the folds must agree
#   with.
#
#   CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/search_oracle.py -- <source folder> <out.json>

import json
import os
import random
import sys
import unicodedata

from calibre.constants import __version__
from calibre.ebooks.unihandecode.unidecoder import Unidecoder
from calibre.utils.icu import primary_contains, primary_no_punc_contains
from calibre.utils.localization import get_all_translators, get_lang

source, out_json = sys.argv[-2], sys.argv[-1]

# The scripts book metadata is written in, up to the half-width forms; the
# Han ideographs and the Hangul syllables are left as they are, as both
# collators leave them.
RANGES = [
    (0x0000, 0x2FFF),
    (0x3000, 0x33FF),
    (0xFB00, 0xFFEF),
]
SKIPPED = [(0xD800, 0xF8FF)]


def assigned(code):
    if any(lo <= code <= hi for lo, hi in SKIPPED):
        return False
    return unicodedata.category(chr(code)) not in ('Cn', 'Cs', 'Co')


def ignored(char, contains):
    return contains('ab', 'a' + char + 'b') and contains('xy', 'x' + char + 'y')


def equal(a, b, contains):
    return bool(a) and bool(b) and contains(a, b) and contains(b, a)


def simple_lower(char):
    lowered = char.lower()
    return lowered if len(lowered) == 1 else char


decoder = Unidecoder()


fold_cache = {}


def fold_of(char, contains):
    key = (char, contains)
    if key in fold_cache:
        return fold_cache[key]
    result = simple_lower(char)
    if ignored(char, contains):
        result = ''
    elif char.isascii():
        # Plain ASCII is its own lower case, or ignored.
        pass
    else:
        candidates = []
        # Unihandecode's ASCII first: "e" for "é", "ae" for "æ", "d" for "ð",
        # "'" for "’".
        ascii_form = decoder.replace_point(char).strip().lower()
        if ascii_form and ascii_form.isascii() and ascii_form != char.lower():
            candidates.append(''.join(fold_of(c, contains) for c in ascii_form))
        # Katakana as hiragana, folded in turn: "は" for "パ".
        code = ord(char)
        if 0x30A1 <= code <= 0x30F6:
            candidates.append(fold_of(chr(code - 0x60), contains))
        # The characters it decomposes to, each folded.
        decomposed = unicodedata.normalize('NFKD', char)
        if decomposed != char:
            candidates.append(''.join(fold_of(c, contains) for c in decomposed))
        for candidate in candidates:
            if equal(candidate, char, contains):
                result = candidate
                break
    fold_cache[key] = result
    return result


folds = []
for lo, hi in RANGES:
    for code in range(lo, hi + 1):
        if not assigned(code):
            continue
        char = chr(code)
        shifted = fold_of(char, primary_no_punc_contains)
        plain = fold_of(char, primary_contains)
        lower = simple_lower(char)
        if shifted != lower or plain != lower:
            folds.append((code, shifted, plain))

fold_map = {code: (shifted, plain) for code, shifted, plain in folds}


def fold(text, plain=False):
    """The folded text, and the places inside a character folded to several:
    a match can neither start nor end there, as ICU's cannot."""
    result = []
    inside = set()
    length = 0
    for char in text:
        entry = fold_map.get(ord(char))
        folded = simple_lower(char) if entry is None else entry[1] if plain else entry[0]
        inside.update(range(length + 1, length + len(folded)))
        length += len(folded)
        result.append(folded)
    return ''.join(result), inside


def contains(query, text, plain=False):
    q = fold(query, plain)[0]
    t, inside = fold(text, plain)
    start = t.find(q)
    while start >= 0:
        if start not in inside and start + len(q) not in inside:
            return True
        start = t.find(q, start + 1)
    return False


def c_string(text):
    escaped = text.encode('utf-8').decode('latin-1')
    result = []
    for ch in escaped:
        code = ord(ch)
        if ch in '"\\?':
            result.append('\\' + ch)
        elif 32 <= code < 127:
            result.append(ch)
        else:
            result.append('\\%03o' % code)
    return '"' + ''.join(result) + '"'


with open(os.path.join(source, 'calibre_search.inc'), 'w', encoding='utf-8') as f:
    f.write('// Generated by tools/oracle/search_oracle.py from Calibre %s; do not edit.\n' % __version__)
    f.write('''// What Calibre's primary collators make of each character that is not
// simply its own lower case, as { code point, fold ignoring spaces and
// punctuation, fold keeping them }, in UTF-8: empty for a character the
// collator ignores. Found by asking Calibre's ICU collators, for its "%s"
// interface language, character by character.

''' % get_lang())
    f.write('const SearchFold searchFolds[] = {\n')
    for code, shifted, plain in folds:
        f.write('    { 0x%04X, %s, %s },\n' % (code, c_string(shifted), c_string(plain)))
    f.write('};\n\n')
    f.write('// The words a date or a true and false search takes, by interface\n')
    f.write('// language: Calibre\'s translations of today, yesterday, thismonth,\n')
    f.write('// daysago, No, no, Yes, yes, unchecked, checked, empty and blank.\n')
    f.write('const SearchWords searchWords[] = {\n')
    words = ('today', 'yesterday', 'thismonth', 'daysago', 'No', 'no', 'Yes', 'yes', 'unchecked', 'checked', 'empty',
             'blank')
    words_by_language = {'en': words}
    for lang, translator in get_all_translators():
        words_by_language[lang] = tuple(translator.gettext(w) for w in words)
    for lang in sorted(words_by_language):
        f.write('    { %s, %s },\n' % (c_string(lang), ', '.join(c_string(w) for w in words_by_language[lang])))
    f.write('};\n')

# Words and texts in many scripts, with the characters the folds are about,
# and every pair of them; then random pieces of the texts as queries.
words = [
    'ecume', 'Écume', "l'écume", 'lecume', "L'Ecume des jours", 'des jours', 'désjours', 'Œuvre', 'oeuvre', 'œ',
    'Straße', 'strasse', 'ss', 's', 'ß', 'Ærø', 'aero', 'ae', 'a', 'æ', 'Łódź', 'lodz', 'Đorđe', 'dorde', 'Þór',
    'thor', 'Ðe', 'de', 'ħ', 'h', 'ı', 'i', 'İ', 'İstanbul', 'istanbul', 'Ñandú', 'nandu', 'ﬁn', 'fin', 'x²', 'x2',
    '½', '1/2', '12', 'Ёлка', 'елка', 'Йога', 'иога', 'йога', 'Ἀθῆναι', 'αθηναι', 'Αθήνα', 'カタカナ', 'かたかな',
    'ばか', 'はか', 'ﾊﾟﾝ', 'パン', 'ぱん', '١٢٣', '123', '①②', '12', 'Mr. Smith', 'mrsmith', 'mr smith', 'mr-smith',
    'C++', 'c', 'c+', 'R&D', 'rd', 'a$b', 'ab', 'a+b', '—', '-', "'", '’', '...', '…', ' ', 'O’Brien', "O'Brien",
    'obrien', 'Saint-Exupéry', 'saintexupery', 'saint exupery', 'Science-Fiction', 'sciencefiction', 'SF', 'sf',
    '50%', '50', '#1', '1', 'naïve', 'naive', 'Zoë', 'zoe', 'Ångström', 'angstrom', 'Ørsted', 'orsted', 'Dvořák',
    'dvorak', 'Wałęsa', 'walesa', 'Čapek', 'capek', 'Gödel', 'godel', 'goedel', 'Müller', 'muller', 'mueller',
    'Ελληνικά', 'ελληνικα', 'Москва', 'москва', '東京', '東', 'בְּרֵאשִׁית', 'בראשית', 'العربية', 'ﺍﻟﻌﺮﺑﻴﺔ',
    '<div><p>Un roman</p></div>', 'div', 'p', 'unroman', 'roman', '"quoted"', 'quoted', '(paren)', 'paren', '[1]',
    'café', 'café', 'Cafe', 'ÉCOLE', 'ecole', 'école', 'ﬀ', 'ff', 'ǅ', 'dž', 'ǆ', 'ĳ', 'ij', 'ſ', 'st', 'ﬅ',
    'Ⅻ', 'xii', '℃', 'c', '°c', '™', 'tm', 'ª', 'º', 'o', 'µ', 'μ', 'u', 'Ω', 'ω', 'Å', 'å', 'K', 'k',
]
queries = sorted(set(w.lower() for w in words) | set(words))
texts = sorted(set(words))
random.seed(7)
pieces = set()
for text in texts:
    for _ in range(3):
        if len(text) > 1:
            start = random.randrange(len(text))
            end = random.randrange(start + 1, len(text) + 1)
            pieces.add(text[start:end])
queries = sorted(set(queries) | pieces)

# For each query, a character per text: whether Calibre finds the query in
# it ignoring punctuation (1 or 0), and keeping it.
cases = []
disagreements = 0
for query in queries:
    shifted = ''.join('1' if primary_no_punc_contains(query, text) else '0' for text in texts)
    plain = ''.join('1' if primary_contains(query, text) else '0' for text in texts)
    cases.append({'query': query, 'contains': shifted, 'accents': plain})
    for text, expected, expected_plain in zip(texts, shifted, plain):
        if contains(query, text) != (expected == '1') or contains(query, text, True) != (expected_plain == '1'):
            disagreements += 1
            if os.environ.get('SEARCH_ORACLE_DEBUG'):
                print('DIS', repr(query), repr(text), expected, expected_plain)

with open(out_json, 'w', encoding='utf-8') as f:
    json.dump({'calibre_version': __version__, 'interface_language': get_lang(), 'texts': texts, 'cases': cases},
              f, ensure_ascii=False, indent=0)
    f.write('\n')

print('%d folds, %d pairs, %d where folding disagrees with Calibre' % (len(folds), len(queries) * len(texts), disagreements))

