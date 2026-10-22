# Writes tests/data/notes/.calnotes: the notes Calibre's own API keeps for a
# copy of the fixture library, on an author, a series and a tag, one with an
# image, as the tests read them:
#
#   calibre-debug -e tools/oracle/notes_fixture.py -- tests/data

import os
import shutil
import sys
import tempfile

from calibre.library import db as open_db

data = sys.argv[-1]
work = tempfile.mkdtemp()
library = os.path.join(work, 'library')
shutil.copytree(os.path.join(data, 'library'), library)
cache = open_db(library).new_api


def item(field, name):
    for item_id, value in cache.get_id_map(field).items():
        if value == name:
            return item_id
    raise SystemExit(f'No {field} named {name}')


cover = os.path.join(library, 'Glen Cook', 'The Black Company (2)', 'cover.jpg')
with open(cover, 'rb') as f:
    image = cache.add_notes_resource(f.read(), 'portrait.jpg')
alg, digest = image.split(':', 1)
cache.set_notes_for('authors', item('authors', 'Glen Cook'),
                    '<p>An American writer of <b>dark fantasy</b>.</p>'
                    f'<p><img src="calres://{alg}/{digest}?placement=0"></p>',
                    searchable_text='An American writer of dark fantasy.', resource_hashes=(image,))
cache.set_notes_for('series', item('series', 'Contes'), '<p>Tales for children, <i>and others</i>.</p>',
                    searchable_text='Tales for children, and others.')
cache.set_notes_for('tags', item('tags', 'Fantasy'), '<p>Worlds that are not ours.</p>',
                    searchable_text='Worlds that are not ours.')
cache.close()
out = os.path.join(data, 'notes', '.calnotes')
shutil.rmtree(out, ignore_errors=True)
shutil.copytree(os.path.join(library, '.calnotes'), out)
shutil.rmtree(work)
