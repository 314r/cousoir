# Writes the part of Calibre's resources the tests read for language names,
# from the Calibre installed, so that the tests do not depend on it:
#
#   calibre-debug -e tools/oracle/language_fixture.py -- tests/data/calibre-resources
#
# localization/iso639.calibre_msgpack holds the English names of a few
# languages, localization/stats.calibre_msgpack the French and German
# translations, and localization/locales.zip French's messages.mo and
# iso639.mo with those names alone, as Calibre ships them.

import io
import os
import struct
import sys
import zipfile

from calibre.utils.localization import _load_iso639
from calibre.utils.resources import get_path as P
from calibre.utils.serialize import msgpack_dumps

CODES = ('eng', 'fra', 'deu', 'spa', 'ita', 'grc', 'tlh', 'lat', 'jpn', 'zho')
LANGUAGES = ('fr', 'de')

out = sys.argv[-1]
localization = os.path.join(out, 'localization')
os.makedirs(localization, exist_ok=True)

iso639 = _load_iso639()
by_3 = {code: iso639['by_3'][code] for code in CODES}
with open(os.path.join(localization, 'iso639.calibre_msgpack'), 'wb') as f:
    f.write(msgpack_dumps({'by_3': by_3}))
with open(os.path.join(localization, 'stats.calibre_msgpack'), 'wb') as f:
    f.write(msgpack_dumps({lang: 1.0 for lang in LANGUAGES}))


def read_mo(data):
    import gettext
    return gettext.GNUTranslations(io.BytesIO(data))


def write_mo(messages):
    # GNU gettext's format, little endian, without a hash table.
    keys = sorted(messages)
    ids = b''.join(k.encode('utf-8') + b'\0' for k in keys)
    strs = b''.join(messages[k].encode('utf-8') + b'\0' for k in keys)
    start = 7 * 4 + 16 * len(keys)
    offsets_ids, offsets_strs, at = [], [], 0
    for k in keys:
        offsets_ids.append((len(k.encode('utf-8')), start + at))
        at += len(k.encode('utf-8')) + 1
    at = 0
    for k in keys:
        offsets_strs.append((len(messages[k].encode('utf-8')), start + len(ids) + at))
        at += len(messages[k].encode('utf-8')) + 1
    header = struct.pack('<7I', 0x950412de, 0, len(keys), 7 * 4, 7 * 4 + 8 * len(keys), 0, 0)
    table = b''.join(struct.pack('<2I', *o) for o in offsets_ids + offsets_strs)
    return header + table + ids + strs


with zipfile.ZipFile(P('localization/locales.zip', allow_user_override=False)) as source, \
        zipfile.ZipFile(os.path.join(localization, 'locales.zip'), 'w') as target:
    for lang in LANGUAGES:
        for catalog in ('messages.mo', 'iso639.mo'):
            translator = read_mo(source.read(f'{lang}/{catalog}'))
            # gettext's header, which names the charset.
            messages = {'': 'Content-Type: text/plain; charset=UTF-8\n'}
            for name in by_3.values():
                translated = translator._catalog.get(name)
                if translated:
                    messages[name] = translated
            target.writestr(f'{lang}/{catalog}', write_mo(messages))
