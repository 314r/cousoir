#!/usr/bin/env python3
# Writes the FB2, FBZ and TXT books of tests/data/import, which try the
# corners of Calibre's FB2 and TXT metadata readers: a full FB2 with a
# cover, a sparse one with a prefixed namespace and no title, an FBZ, a
# text file with a title and authors in its first lines and one without.
# tools/make_import_fixtures.sh runs it; tests/golden/run.sh reads them with
# Cousoir and with Calibre.
#
#   python3 tools/make_text_fixtures.py tests/data/import COVER.jpg

import base64
import os
import sys
import zipfile

out, cover = sys.argv[1], sys.argv[2]
with open(cover, 'rb') as f:
    picture = base64.encodebytes(f.read()).decode('ascii')

full = f'''<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
 <description>
  <title-info>
   <genre>prose_classic</genre>
   <genre>love_history</genre>
   <author><first-name>Marie-Henri</first-name><middle-name>dit</middle-name><last-name>Beyle</last-name></author>
   <author><nickname>Stendhal</nickname></author>
   <book-title>Le Rouge  et le
     Noir</book-title>
   <annotation><p>Chronique du XIXᵉ siècle.</p>
<p>Julien Sorel &amp; Mme de Rênal.</p></annotation>
   <coverpage><image l:href="#cover.jpg"/></coverpage>
   <lang>fr</lang>
   <sequence name="Chroniques" number="1 5"/>
  </title-info>
  <document-info>
   <author><nickname>scanner</nickname></author>
   <program-used>Hand</program-used>
   <date>2024</date>
   <id>rouge-1</id>
   <version>1.0</version>
  </document-info>
  <publish-info>
   <book-name>Le Rouge et le Noir</book-name>
   <publisher>Levasseur</publisher>
   <city>Paris</city>
   <year>1830</year>
   <isbn>978-2-07-036024-6, 0000</isbn>
  </publish-info>
 </description>
 <body>
  <section>
   <title><p>Chapitre premier</p></title>
   <p>La petite ville de Verrières peut passer pour l’une des plus jolies de la Franche-Comté.</p>
  </section>
 </body>
 <binary id="cover.jpg" content-type="image/jpeg">{picture}</binary>
</FictionBook>
'''

sparse = '''<?xml version="1.0" encoding="windows-1251"?>
<fb:FictionBook xmlns:fb="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:xlink="http://www.w3.org/1999/xlink">
 <fb:description>
  <fb:title-info>
   <fb:book-title>   </fb:book-title>
   <fb:sequence name="Серия без номера"/>
  </fb:title-info>
  <fb:src-title-info>
   <fb:genre>sf</fb:genre>
   <fb:author><fb:first-name>Иван</fb:first-name><fb:last-name>Петров</fb:last-name></fb:author>
   <fb:annotation>Аннотация <fb:emphasis>без</fb:emphasis> абзаца.</fb:annotation>
  </fb:src-title-info>
  <fb:publish-info>
   <fb:year>1999.5</fb:year>
   <fb:isbn>12345</fb:isbn>
  </fb:publish-info>
 </fb:description>
 <fb:body><fb:section><fb:p>Текст.</fb:p></fb:section></fb:body>
</fb:FictionBook>
'''

with open(os.path.join(out, 'Le Rouge et le Noir - Stendhal.fb2'), 'w', encoding='utf-8') as f:
    f.write(full)
with open(os.path.join(out, "Sans titre - Quelqu'un.fb2"), 'w', encoding='windows-1251') as f:
    f.write(sparse)
with zipfile.ZipFile(os.path.join(out, 'Rouge.fbz'), 'w', zipfile.ZIP_DEFLATED) as z:
    info = zipfile.ZipInfo('rouge.fb2', date_time=(2024, 1, 1, 0, 0, 0))
    z.writestr(info, full.replace('Le Rouge  et le', 'Le Rouge, zippé, et le'))
with open(os.path.join(out, 'Moby.txt'), 'w', encoding='utf-8', newline='') as f:
    f.write('Moby Dick\n\n\nHerman Melville, Ishmael\nCall me Ishmael. Some years ago, never mind how long precisely.\n')
with open(os.path.join(out, 'Notes - Anonyme.txt'), 'w', encoding='utf-8', newline='') as f:
    f.write('Des notes sans titre.\n\nUne autre ligne.\n')
