#!/usr/bin/env python3
# Writes tests/data/reader/cfi.epub, pages made to trip a CFI worked out
# from the XML rather than from the browser's document: text straight in the
# body, comments, an instruction and CDATA between texts, an element first,
# texts after blocks, unique, repeated and odd ids, entities, characters
# beyond the BMP, SVG and MathML, a page with CRLF line ends and a text
# longer than a browser keeps in one node.
#
#   python3 tools/make_cfi_fixture.py tests/data/reader/cfi.epub

import sys
import zipfile

XHTML11 = ('<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" '
           '"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">\n')


def page(title, body, doctype=''):
    return ('<?xml version="1.0" encoding="utf-8"?>\n' + doctype
            + '<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">\n'
            '<head><title>' + title + '</title></head>\n<body>' + body + '</body>\n</html>\n')


MIXED = page('Mixed', '''Text straight in the body, before any element.
<h1 id="top">Mixed content</h1>
<p>Plain text with <em>emphasis</em> and <strong>strong <em>nested</em> words</strong> after them.</p>
<p><b>Bold first</b> then the text after a leading element.</p>
<p>Before a comment<!-- a comment --> after the comment<?cousoir an instruction?> after the instruction.</p>
<p>Text and <![CDATA[a CDATA section with <angle> brackets]]> then text again.</p>
<div>Text in a division <p>a paragraph in it</p> text after the paragraph <span>and a span</span> the end.</div>
<p id="unique">A unique id.</p>
<p><span id="dup">One duplicate</span> and <span id="dup">another duplicate</span>.</p>
<p id="a[b]">An id with brackets.</p>
<p id="x,y">An id with a comma; (parentheses) ^caret ~tilde @at !bang.</p>
<p>Entities: caf&eacute; &amp; cr&egrave;me&nbsp;br&ucirc;l&eacute;e &lt;tag&gt; &#x1F600; &#233;.</p>
<p>Beyond the BMP: \U0001F600 \U0001D11E music, \U0001F1EB\U0001F1F7 flag.</p>
<p>Line<br/>break and <a id="link" href="#top">a link</a> in it.</p>
<table><tr><td>cell one</td><td>cell <i>two</i></td></tr></table>
<ul><li>first item</li><li>second <em>item</em></li></ul>
<pre>  preformatted
    text  </pre>
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="20"><text x="0" y="15">SVG text</text></svg>
<math xmlns="http://www.w3.org/1998/Math/MathML"><mi>x</mi><mo>=</mo><mn>2</mn></math>
<p>   </p>
<p>Last paragraph.</p>
''', XHTML11)

NESTED = page('Nested', '''<div><div><p>Deep <em>text <b>deeper</b> still</em> out.</p></div></div>
<section><h2>A section</h2><p>Its paragraph, <span><span>two spans</span></span> in.</p></section>
<blockquote><p>A quotation.</p>And text after it.</blockquote>
''')

CRLF = page('Line ends', '''
<h2>Lines</h2>
<p>A paragraph
written over
several lines.</p>
<p>Another one.</p>
''', '<!DOCTYPE html>\n').replace('\n', '\r\n')

WORDS = 'river stone lantern harbour quiet north letter window garden morning'.split()
LONG = page('Long', '<p>' + ' '.join(WORDS[i % len(WORDS)] for i in range(12000)) + '</p>\n'
            '<p>A short one <em>after</em> it.</p>\n')

PAGES = [('mixed.xhtml', 'Mixed', MIXED), ('nested.xhtml', 'Nested', NESTED),
         ('crlf.xhtml', 'Line ends', CRLF), ('long.xhtml', 'Long', LONG)]

with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr(zipfile.ZipInfo('mimetype'), 'application/epub+zip')
    z.writestr('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" '
               'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
               '<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
               '</rootfiles></container>')
    items = ''.join(f'<item id="p{i + 1}" href="text/{name}" media-type="application/xhtml+xml"/>'
                    for i, (name, _, _) in enumerate(PAGES))
    spine = ''.join(f'<itemref idref="p{i + 1}"/>' for i in range(len(PAGES)))
    z.writestr('OEBPS/content.opf', '<?xml version="1.0" encoding="utf-8"?>'
               '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
               '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:uuid:7d0a3c6e-1d1e-4b8e-9b54-0000000000cf</dc:identifier>'
               '<dc:title>The Odd Pages</dc:title><dc:creator>Ann Example</dc:creator><dc:language>en</dc:language>'
               '<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata><manifest>'
               '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
               + items + '</manifest><spine>' + spine + '</spine></package>')
    toc = ''.join(f'<li><a href="text/{name}">{title}</a></li>' for name, title, _ in PAGES)
    z.writestr('OEBPS/nav.xhtml', '<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" '
               'xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body>'
               '<nav epub:type="toc"><ol>' + toc + '</ol></nav></body></html>')
    for name, _, content in PAGES:
        z.writestr('OEBPS/text/' + name, content.encode('utf-8'))
