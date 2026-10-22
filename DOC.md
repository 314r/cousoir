# Cousoir, in detail

Everything about Cousoir: each screen and shortcut, how it reads and writes a
Calibre library, where it departs from Calibre, `cousoir-cli`, the build and
the tests. The [README](README.md) has the short version.

Cousoir is a Qt/QML ebook manager that works on a Calibre library: the books,
sorted and searched as a table or as covers, the authors, series, tags,
languages and publishers that browse them, and each book's metadata,
description and files, which open in the desktop's reader. A book's metadata
and cover edit in place; books and files are added, moved to the library's
trash and restored from it; Calibre converts books to KEPUB for a Kobo and
AZW3 for a Kindle; books go to a Kobo or a Kindle plugged in, and come off
it, and what a Kobo knows of their reading, highlights and notes included,
comes back. Ctrl+K finds a book, an item, a section or an action by a few
letters. Calibre's templates run, for composite columns and searches.

Calibre stays the library's owner and keeps working as before. Cousoir reads
and writes Calibre's own `metadata.db` and, wherever it computes something
Calibre computes, follows Calibre's rules, ported from Calibre's own code, so
both always agree; the tests compare its writes with Calibre's own. Calibre
itself converts the books and sends them to the readers.
[CREDITS.md](CREDITS.md) lists the Calibre code in Cousoir, file by file.

The project structure, theming (system, light, dark and Omarchy) and Settings
window come from the JolieBulle Qt port, which took them from kbextract; the
Omarchy theming is derived from bjarneo's omadji.

## Interface

The interface follows the Omarchy style: the system monospace font, square
corners, hairline dividers, foreground-tinted hover and selection states
instead of accent fills, and one accent per screen.

The sidebar has the books, the trash and the check, then the browse sections
with their sizes: authors, series, tags, languages and publishers, then the
Kobos and Kindles plugged in. A section lists its items with their number of
books; Enter or a double click lists an item's books, with the item in the
breadcrumb. Esc, Alt+Left or the back button goes back up one level.

The books show as a table or, with Ctrl+G or the toolbar button, as a grid of
covers; both keep the same book selected. Click a column's header to sort,
and click it again to reverse the order. The list sorts as Calibre's does
(`multisort()`), each field by the keys Calibre makes of it: titles by their
title sort, "Petit Prince, Le", authors by their author sort, a series by its
name sorted as a title in the book's language then the book's number, tags
and languages by their sorted names, formats by how many a book has, dates to
the second, a book without a number as the tweak says. Books alike keep the
order the sorts before gave them, Calibre's sort history: sorted by author
after a sort by title, an author's books come by title. The golden test
compares 90 sorts with Calibre's on three libraries.

A right click on a header shows, hides or moves its column and sorts by it;
any of Calibre's columns can show: title, authors, series, rating, tags,
publisher, published, added, modified, size, languages, formats,
identifiers, pages, id, path and the library's custom columns. The columns
and the sort are kept for each library; the first time, they are those
Calibre's list last had for it, if Calibre saved them. Columns leave a narrow
window from the right. The search, Ctrl+F, is Calibre's: see
[Searching](#searching). In a browse section, it finds the items whose name
has every word typed.

A book opens with Enter, Space or a double click. Its page shows the cover,
the title, authors, series and rating, and the description, beside a details
pane with the rest of its metadata, the library's custom columns, its
identifiers (some link to their page) and its files. Below 960 px of window width the pane hides; the status bar
toggle (Ctrl+I) shows it anyway. A file's button, or Open in reader (Ctrl+O),
opens it with the desktop's application for that format; Open in reader picks
EPUB, KEPUB, AZW3, MOBI or PDF, in that order. Ctrl+O also works on the
selected row of a list. A row's menu (right click, Menu or Shift+F10) opens
the book, opens it in the reader, shows its folder, converts it, sends it to
a Kobo or a Kindle or moves it to the trash.

When the library cannot be written right now, the status bar says why with a
lock: Calibre open on this computer, Calibre writing, Dropbox syncing or not
running, Cousoir editing on another computer, a change in the last two
minutes to a library shared with other computers, or a conflict copy of the
database. A click on the lock opens the library settings.

A library is shared with other computers when it is in a Dropbox folder
(named in Dropbox's `info.json`), in a Syncthing folder (a `.stfolder` in
the library or a folder above it) or a Nextcloud or ownCloud one (their
`.sync_*.db` journal), or on a network file system: NFS, SMB, sshfs, rclone,
GVfs or a virtual machine's shared folder. Anything else counts as only this
computer: no wait after a change, as nothing but Calibre here can make one,
and Calibre is seen. The library settings say what Cousoir found and correct
it for a library shared some other way, OneDrive or Google Drive for
instance, or one only this computer changes despite a sync folder; the
choice is kept per library and `cousoir-cli` follows it. With Only this
computer in a Dropbox folder, only a Dropbox sync under way still waits.

## Finding with Ctrl+K

Ctrl+K opens a palette over the window. A few letters find the library's
books, by their title or their title and authors together, its authors,
series, tags and publishers, and the window's sections, virtual libraries
and the actions that can be done now, each with its shortcut: add books,
open in the reader, convert, send to or eject a reader, move to the trash,
show the covers or the details, reload, settings, the themes. Letters match
as the search matches them, without case, accents or punctuation; what is
the whole name typed, or starts with it, comes first, then commands, then
items, then books. Up and Down choose, Enter goes to the book, lists the
item's books or does the action, Esc closes. Before anything is typed, it
lists the sections and the actions.

## Templates

Calibre's template language is ported from Calibre's own code
(`utils/formatter.py` and `formatter_functions.py`): the single function
mode, `{title} by {authors}`, `{series_index:0>2s}`, `{series:|[|]}`,
`{tags:sublist(0,1,\,)}`, `{title:'uppercase($)'}`; and the program mode,
`program: if $series then strcat($series, ' #', $series_index) fi`, with
its operators, `if`, `for`, `def`, `return`, globals and `with`, and the
functions Calibre has. A book's fields read as Calibre shows them to a
template, dates in the format of each field and the names of months in
Calibre's interface language.

Composite columns are computed with it when the library loads, and again
for the books a change writes the `metadata.opf` of: they show in the
details pane, the lists and the OPF files as Calibre computes them, and
searches find them. `template:` searches run a template on each book. The
golden test runs a battery of templates through Calibre's formatter and
Cousoir's, on three libraries, with the same results.

Cousoir departs from Calibre here:

- Python templates, `python:`, are not run; they fail.
- The functions that need Calibre's window fail, as they do in `calibredb`:
  `virtual_libraries()`, `annotation_count()`, `book_count()`, notes and
  extra files among them; `user_categories()`, `get_link()` and
  `author_links()` give nothing.
- The template functions a library stores, Calibre's "Stored templates",
  are not read: a template calling one fails.
- An error says what went wrong in English, after Calibre's "TEMPLATE
  ERROR" in Calibre's language.
- A date given to `format_date()`, `days_between()` and the like reads as
  ISO dates do, or as day, month and year with English month names, the
  ones Calibre's dateutil reads most.
- A template that calls itself, or nests too deep, stops with an error
  past a depth of its own, where Calibre stops at Python's limit.
- A program whose value is `$` alone, in a composite or a search where
  Calibre's `$` is None, gives the empty text, where Calibre fails.
- Integers that do not fit 64 bits are an error in a `for` range and in
  `range()`; `ceiling()`, `floor()`, `round()` and `mod()` print every
  digit, as Python does.
- `urls_from_identifiers()` makes the links of the metadata sources Calibre
  9.16 has, Google, Amazon and Edelweiss, then ISBN, DOI, arXiv, OCLC, ISSN
  and URL ones and the links set in Calibre's preferences; it sorts them
  case and accents aside as Calibre's primary sort key does. An entity in
  an identifier, `&amp;`, stays as it is.

## Language names

Languages show, sort and search by their names in Calibre's interface
language, "Anglais" for English in French, as Calibre names them: from its
ISO 639 table and its translations, read from the Calibre installed. Without
Calibre, names are English. A search finds a language by its code, its
English name or its name in Calibre's language.

## Searching

The search box takes Calibre's search language and finds the books Calibre
finds; the golden test checks it on a battery of searches against Calibre's
own. Words are looked for in every field, each one somewhere, ignoring case,
accents, spaces and punctuation as Calibre does: `ecume` finds "L'Écume
des jours". Then, as in Calibre:

- `title:prince`, `author:vian`, `tags:fantasy`, `series:`, `publisher:`,
  `comments:`, `formats:epub`, `languages:fra` or `languages:french`,
  `author_sort:`, `title_sort:`, `series_sort:`, `uuid:`; `"quoted words"`;
- `=` for an exact value, `tags:=fiction`, and `tags:=.fiction` for a tag
  and the tags under it; `~` for a regular expression, `title:~^the`; `^`
  to keep punctuation;
- `and`, `or`, `not` and parentheses, `and` when nothing is written;
- `rating:>=4` in stars, `size:>1m`, `id:<100`, `series_index:2`,
  `pages:>300`, `cover:true`; `date:2020`, `pubdate:<1900`,
  `last_modified:>2024-03`, `date:today`, `date:<30daysago`, `thismonth`;
- `true` and `false`, `series:false` for a book in no series; `#` counts,
  `tags:#>3`, `formats:#=0`;
- `identifiers:isbn:978`, `isbn:true`, `identifiers:goodreads:false`;
- custom columns by their search term, `#genre:sf`, `#pagecount:>300`,
  `#read:false`, `#started:2024`, `#saga_index:2`, `#myrating:>=4` in
  stars; a composite column as text, or as a number or a date when its
  sort says so;
- `template:"{series} {title}#@#:t:omens"`: a template's text for each
  book, compared as text (`t`), a number (`n`), a date (`d`) or yes or no
  (`b`), see [Templates](#templates);
- `search:"name"` for a saved search, `vl:name` for a virtual library's
  books, the library's grouped search terms, and `@category:true` for its
  user categories.

A search Calibre cannot read, a parenthesis missing for instance, lists
nothing and says why.

The saved searches of the library, which Calibre keeps in `metadata.db`,
show in the sidebar under Searches, each with its count; one lists its
books like a section, and the search box searches within them. The
library's virtual libraries are in the box beside the search: one narrows
every section, its counts and the saved searches' to its books, as in
Calibre. Cousoir remembers the choice per library; until one is made, it
opens with the virtual library Calibre opens the library with.

Cousoir departs from Calibre's search in a few places:

- How text matches was taken from Calibre's collator for French, which
  English and most other languages share; Calibre in Swedish, say, also
  tells "a" from "å" in a search.
- Regular expressions are PCRE's, Calibre's Python's: the common ones are
  the same.
- A date is a year, a month or a day, `2020-03-15`, or a day and month in
  the system's order; Calibre also reads "March 2020".

## Editing

While the library can be written, the details pane of a book edits its title,
title sort, authors, author sort, series and number, tags, rating, publisher,
languages, publication date and identifiers. A field saves when you leave it,
with Enter, Tab or a click elsewhere; Esc puts the stored value back; a value
the field cannot take, an empty title or a date it cannot read, shows in red
and is not saved. Authors are separated by `&`, tags and languages by commas;
languages take names or codes; identifiers read `isbn:…, goodreads:…`. A date
takes `1947-03-20`, `1947-03` or `1947`, as Calibre reads them; an empty one
clears it.

The library's custom columns follow, each by its name: text, several values
separated by commas (`&` for names), numbers, dates and long text in a
field, a series with its number below it, and ratings, yes or no and lists
of fixed values as a choice. A value is saved as Calibre's writers save it:
a number Calibre cannot read shows in red, a series written `Cycle [3]`
takes its number, and a list of fixed values takes only those. A long text
holding more than text, a table or an image, is read-only there and says
so, as saving it from its text would leave that out. A composite column
shows as its template makes it, and cannot be edited.

As in Calibre's editor, a title sort that followed the title keeps following
it, with the book's language for its articles, and an author sort that
followed the authors keeps following them; one you set by hand stays. A
changed title or first author moves the book's folder and renames its files,
exactly as Calibre does.

An image file dropped on the cover replaces it, as does the Cover menu under
it, which also pastes an image (Ctrl+V on the book page) or removes the cover.
A JPEG that fits in 1650 by 2200 pixels is kept as it is; anything else is
scaled to fit and saved as a JPEG, its transparency on white, as Calibre
saves covers.

Edit, beside the description's heading, opens it as text, in the book's
page: paragraphs apart by a blank line, a line break where a line ends,
`**bold**`, `*italics*`, `[a link](https://…)`, `- ` or `1. ` for a list
and `#` for a heading. Ctrl+Enter or Save saves it as HTML in paragraphs,
as Calibre keeps it; Esc or Cancel puts it back; closing the book saves it.
A description with more than that, a table or an image, says that saving it
from there leaves that out; one saved unchanged is not rewritten.

Once the open book has changed, Revert changes in the toolbar puts it back as
it was when you opened it, cover and description included, after asking
(Enter reverts, Esc keeps). Closing the book makes its changes final.

### How a change is saved

Each change is saved at once, one after the other, on a worker thread:

1. Cousoir takes Calibre's own lock on its libraries, so neither the Calibre
   GUI nor calibredb can start on this computer meanwhile.
2. It reads `metadata.db`, keeping a copy of it before the session's first
   change in `~/.local/share/cousoir/backups/<library>/` (the last 30).
3. The change goes to a copy in the cache, through a port of Calibre's field
   writers: the same cleaning of values, the same case-insensitive matching of
   authors, tags, series and publishers, the same removal of items no book
   uses any more, `last_modified` and the queue of books whose
   `metadata.opf` Calibre writes again.
4. Folders move, files are renamed, covers and `metadata.opf` files are
   written, each step logged first to a journal in the cache, with every
   replaced file kept beside its place under a `.~` name Dropbox ignores.
5. If `metadata.db` changed meanwhile, Dropbox bringing another computer's
   change, the files are put back and the change starts again on the new
   database.
6. The copy replaces `metadata.db` in one rename, keeping its permissions and
   Dropbox's attributes. Dropbox sees one changed file.

A crash leaves the journal: the next start puts the library back as it was,
or finishes the change if the new database was already in place, and says so.

A change that sets values the book already has writes nothing. Cousoir's own
change does not start the two-minute wait, which only a shared library has,
and Dropbox sending it does not make the library read-only. After a change,
Cousoir leaves a heartbeat in a `.cousoir` folder at the root of the Dropbox,
Syncthing or Nextcloud folder: for ten minutes, the Cousoir of another
computer sharing the library stays read-only. When the Syncthing folder is
the library itself, Calibre's Check library lists `.cousoir` as an extra
folder, as it lists `.stfolder`; Nextcloud carries it only if it syncs
hidden files. A network folder gets no heartbeat: close Cousoir and Calibre
on the other computer before editing.

Cousoir departs from Calibre on purpose in a few places:

- An item with a note in `.calnotes` stays when no book uses it any more;
  Calibre removes it and retires the note.
- When a book's folder would move onto a folder that is not empty, Cousoir
  stops; Calibre would delete that folder.
- A database whose authors, tags, series or publishers differ only by case is
  left for Calibre to tidy: Cousoir refuses to write those fields.
- With Calibre in Japanese, Korean or Vietnamese, whose folder names come
  from other tables, Cousoir does not write.

## Several books at once

A click selects a book; Ctrl with a click adds a book to the selection or
takes it out, Shift with a click or with the arrows, Page Up, Page Down,
Home or End selects every book from the last one clicked, and Ctrl+A
selects them all, in the table or the covers. Esc goes back to the current
book alone. The status bar counts them. A sort keeps the books selected; a
search keeps those it still shows.

The row menu, Del, Ctrl+D and Ctrl+Shift+K or Ctrl+Shift+A then act on all
the books selected: moved to the trash in one change, after asking; sent to
the reader, those it reads none of the formats of converted first, after
asking; converted one after the other.

Beside the list, a pane edits them together, as Calibre's bulk metadata
edit does (Ctrl+I hides it). A field filled in goes to every book; one left
empty keeps each book's own; × beside a field clears it for all. The
authors replace each book's, with their author sort; tags are added to and
removed from each book's own, an added tag keeping the library's spelling
of it; a series puts the books at 1, numbers them from a number in the
list's order, or after the series' last book, as Calibre's numbering does;
the rating, publisher, languages and publication date replace each book's.
Apply saves it all in one change; Undo puts the books back as they were
before it, in the fields it could change.

## Adding and removing books

Ctrl+N, or the + button of the toolbar, picks book files to add; files dropped
on the book list are added too, and a folder dropped there adds the book files
in it, those with an extension Calibre takes for a book. Each file becomes a
book. Cousoir reads the files on a worker thread, the status bar counting
them, and reads what `calibredb add` reads:

- the title and authors in the file's name, by Calibre's file name pattern
  (`Title - Author.epub` by default, or the one set in Calibre's preferences);
- then the file's own metadata, unless Calibre is set not to read it, merged
  as Calibre merges it: the file's title and authors unless they are
  "Unknown", its other fields where it has them, its tags with the name's, its
  cover. EPUB and KEPUB (OPF 2 and 3), MOBI, AZW, AZW3 and PRC are read by
  Cousoir itself; PDF through poppler's `pdfinfo` and `pdftoppm`, Calibre's
  own copy when Calibre is installed, its XMP packet merged with its info
  dictionary as Calibre merges them; any other format, and the cover of an
  EPUB whose first page must be drawn, through Calibre itself when it is
  installed.

A book added gets the values its file holds for the library's custom
columns, those of the same name and kind, as Calibre wrote them into an
EPUB or a PDF it saved or exported, and, where it has none, the values the
library's columns give new books, a date of "now" being the time it is
added. A value its column cannot take is left out, as in Calibre.

When books are already in the library by title and authors, as Calibre
compares them (lower case, without punctuation or a leading article, every
author found, the languages alike if both have some), Cousoir asks whether to
add them anyway; the other books are added either way. The books then go into
the library in one change, saved as an edit is, with the tags Calibre gives
new books, their series number from Calibre's rule for the next one, and their
files named and filed exactly as Calibre files them. The first one added is
selected.

On a book's page, the Files section adds a file as one of its formats, after
asking when the book has a file of that format already, and moves a file to
the trash. A file dropped on the book's page is added as a format; an image
dropped there becomes the cover.

Del on the list, the row menu or the toolbar's trash button on a book's page
moves the book to the library's trash, after asking. As in Calibre, its
`metadata.opf` is written again first, its folder goes whole to
`.caltrash/b/<id>`, and the authors, tags, series and publishers no other book
has are removed; a file goes to `.caltrash/f/<id>/<format>` beside a
`metadata.json` with the book's title and authors.

The Trash section lists what the library's trash holds, removed books and
files removed from a book, with the date they were removed and the days left
before Calibre deletes them. Restore puts a book back under its own number,
with its metadata from its `metadata.opf`, its files and cover, or puts the
files back into their book. Cousoir restores what Calibre moved to the trash
and Calibre restores what Cousoir did.

Cousoir departs from Calibre here too:

- It never empties the trash. Calibre deletes entries older than 14 days, or
  its library's own setting, when it starts.
- It does not restore a book whose `metadata.opf` holds annotations. Calibre
  does.
- A PDF's metadata written by Calibre, its custom columns, rating and series,
  is read as Calibre's Add books reads it. `calibredb add` misses it: once
  Calibre's database code is loaded, its XMP reader no longer finds Calibre's
  own names.
- Calibre's file type plugins do not run: an HTML file is added as HTML where
  Calibre would zip it with its images.
- An OPF file named like the added file, beside it, is not read; Calibre
  would take its metadata.
- An old MOBI file without an EXTH header keeps the title of its header,
  where Calibre would also look for metadata in its text.
- Page counts are left for Calibre, which counts the pages of the books
  Cousoir added or changed the next time it opens the library.

## Checking the library

The Check section lists what a library collects over time, and the sidebar
counts it. Cousoir looks after each load, after each change and each time the
section opens, on a worker thread; F5 or Check again looks again.

- **Duplicate books.** Books with the same title and authors, as Calibre
  compares them when adding (`find_identical_books()`: lower case, without
  punctuation or a leading article), accents and the authors' name variants
  aside, unless both have languages and they differ; and books with the same
  ISBN, ISBN-10 and ISBN-13 alike. The same title by other authors, or by
  more, is another book. Keep this one keeps a copy and moves the others to
  the trash, once the formats it has no file for are copied to it from them,
  as Calibre's Merge only formats does: Restore in the Trash undoes it.
- **Author name variants.** One person under several names. The same words
  in another order, case, accents or punctuation aside, "Vian, Boris" for
  "Boris Vian", are the same name written differently. Initials, "B. Vian"
  for "Boris Vian", make it possibly the same person, and only when they fit
  one name of the library: "J. Smith" between "John Smith" and "Jane Smith"
  stays apart. Use this name files their books under the name picked, in one
  change, the book folders moving to match; only editing the authors again
  undoes it. While two author names differ only by case, Calibre has to merge
  them first, which it does the next time it saves an author: Cousoir
  changes no author until then.
- **Books without files.** A format whose file is gone, or a book with no
  file at all. Remove missing entries takes the formats out of the database;
  nothing is deleted from the disk. A Nextcloud or ownCloud placeholder for a
  file kept online counts as the file. In a shared library, a file still on
  its way from another computer looks missing: let the sync finish first.
- **Files outside the database**, as Calibre's Check library finds them:
  files at the top of the library, author folders without a book of the
  library, book folders the database does not know and unknown files in a
  book's folder. Hidden files, Calibre's own, the sync services' (conflict
  copies included), `Thumbs.db` and `desktop.ini` are left out. Show in
  folder shows them; Cousoir leaves them where they are, as a folder from
  another computer may be a book whose database has not arrived yet.

Ignore leaves a finding out until it changes, for a book kept without a file
on purpose or two people who share a name; Show ignored brings them back.
The fixes, like any change, wait for the library to be writable, and say so.
Enter, Menu or Shift+F10 opens a finding's actions.

## Downloading metadata and covers

The globe of a book's toolbar, or "Download metadata and covers" in Ctrl+K,
asks Calibre's metadata sources about the open book, by its title, authors
and identifiers, as Calibre's Edit metadata dialog does: Calibre runs its
own `identify()` in a process of its own, with the sources it is set to use
and their latest versions, and the books found come back in its order. The
one picked shows beside the book's own values, field by field; the fields
that differ are ticked, but for those Calibre is set to leave aside
(Preferences, Metadata download, the fields to download). Covers shows the
covers the sources have for it, the book's own first and kept unless
another is picked, as they come and then largest first. Save takes the
fields ticked, and the cover picked, as one change: Revert changes undoes it
while the book is open. Space ticks the field it is on and the arrows pick
a cover; Esc closes and stops the download.

What a field takes is what Calibre's dialog takes from a download
(`update_from_mi()`): the title, its sort made again in the book's language;
the authors, their sort from the library's authors or made again; the tags
found the book lacks, before its own; the identifiers found added to the
book's; the rating in whole stars; the day published, at midnight here;
the series and its number; the languages; the description, after the
book's own when Calibre is set to add it. The golden test runs Cousoir's
identify job under Calibre on made-up results, applies them through
Calibre's own dialog, shown offscreen, and through Cousoir: the libraries
end alike.

Without Calibre, the dialog asks [Open Library](https://openlibrary.org),
free and without an account: by the book's ISBN, else, or when Open
Library does not know the ISBN, by the words of its title and first
author that Calibre's sources search for. Each work found comes with the
edition that best matches (in the book's language first): its title
(the work's, better accented and cased, unless the edition's is
another), authors, publisher, date, languages and ISBN; the work's
description, from Markdown; its subjects as tags and its series, without
its number, which the book keeps. The results are ranked as Calibre ranks
a source's: a shared identifier, then a cover, every field, the
interface's language, the same title, a longer description. Covers shows
the edition's and the work's covers, six at most. Calibre's download
preferences still apply when Calibre left some (the fields left aside,
the description added to the book's, the most tags). `cousoir-cli
download` does the same without Calibre.

The tests never go on the web: a stand-in for Calibre answers them, and a
server of the tests' own plays Open Library with answers it once gave
(`tools/make_openlibrary_fixtures.sh`).

Cousoir departs from Calibre here:

- Each field is ticked or not for this book, where Calibre leaves aside the
  fields its preferences name for every download.
- The description found is kept as the source gives it; Calibre's editor
  rewrites its HTML first.
- The author sort comes with the authors: a sort found is left with them.

## Converting books

Without Calibre, Convert to KEPUB still works on a book with an EPUB:
Cousoir makes the KEPUB itself, as Calibre's KEPUB conversion of an EPUB
does (`ebooks/oeb/polish/kepubify.py`): each sentence of each page in
Kobo's numbered spans, the body in Kobo's `book-columns` and `book-inner`
divisions, Kobo's styles and script, a title page when the book opens on
none, and the library's metadata and cover in the copy. Its pages come out
byte for byte as Calibre's. `cousoir-cli kepubify BOOK.epub BOOK.kepub` does
it on a file.

The Convert to buttons of the Files section of a book's details, the row menu
or Ctrl+Shift+K and Ctrl+Shift+A convert the book to KEPUB, for a Kobo, or
AZW3, for a Kindle; Cousoir asks first when the book has a file of that
format. Conversions run one after the other, each once the changes before it
are saved; the status bar and the Files section show how far it got, and Stop
in the Files section or the × of the status bar stops it. The converted file
goes into the library as one of the book's formats.

Calibre converts, as its convert action does when a format is picked for a
book and its settings are kept: Calibre's defaults (Preferences, Common
options and Output options) under the settings the book's last conversion
in Calibre saved; the format Calibre would convert from, the one it last
converted from or else the first of its input format order; the library's
metadata and cover put into the new file; the conversion in a Calibre worker
process of its own. The file is added as Calibre adds it, and the settings
are saved in `conversion_options` for the book's next conversion, as
Calibre saves them; a format converted to itself keeps its old file as
`ORIGINAL_AZW3` or `ORIGINAL_KEPUB`, as in Calibre. Without Calibre, only
Convert to KEPUB of a book with an EPUB shows, which Cousoir does itself;
AZW3, and KEPUB from other formats, need Calibre.

Cousoir departs from Calibre here too:

- It has no conversion dialog. The settings it uses are the ones set in
  Calibre: its defaults, or a book's own, saved by converting that book in
  Calibre.
- Several books convert one after the other, each with its own settings, as
  Calibre's "Convert individually" with its dialogs left as they open.
  Calibre's bulk conversion applies one set of settings to all.
- The settings are saved only with the converted file; Calibre saves them as
  its dialog closes, even when the conversion then fails.
- KEPUB and AZW3 only. The quick KEPUB Calibre makes from an EPUB when it
  sends it to a Kobo comes with sending, below.

## Sending to a reader

A Kobo plugged in and mounted, or a Kindle plugged in that MTP reaches, as a
Paperwhite does, shows in the sidebar under Devices, with the name Calibre
gave it or its own. Its page lists the books on it, with their format, size
and whether each is one of the library's, which Enter opens. Del removes a
book from the reader, after asking. The status bar shows the room left on
it.

The book's Files section has a Send to button for each reader plugged in, and
says which readers have the book; the row menu and Ctrl+D, for the first
reader, send too. A book with none of the formats the reader reads is
converted first, after asking, to KEPUB for a Kobo or AZW3 for a Kindle, then
sent, as Calibre offers to. Books go one batch after the other, each once the
changes before it are saved; the status bar and the Files section show how
far it got, and Stop in the Files section or the × of the status bar stops
it.

Calibre sends, as its "Send to device" does with the settings of its driver
for the reader: the format the driver prefers that the book has; the
library's metadata and cover put into a copy of the file; the file placed by
the driver's save template; the book added to `metadata.calibre`, which
tells Calibre the books it sent and their library uuid, and
`driveinfo.calibre` updated. Calibre recognises the books Cousoir sent as the
library's, and Cousoir the books Calibre sent, by their uuid, then by their
title and authors as Calibre does. Calibre does the work through
calibre-debug, on copies of what it reads of the library and of the reader;
Cousoir then puts the new files on the reader, the books first and
`metadata.calibre` last. Without Calibre, Cousoir sends to a Kobo or a
Kindle by itself (below). While Calibre is open on this computer, it handles the readers:
Cousoir sends nothing and removes nothing until it is closed. There is no
settings page: the drivers' settings are Calibre's, set in Calibre.

### Kobo

A Kobo's page lists the books sent by Calibre or Cousoir, those copied by
hand and those bought from Kobo, which stay when the others are removed.
Eject unmounts the Kobo and powers it off, so it can be unplugged. Calibre's
Kobo driver sends KEPUB, then EPUB, then PDF and the others; makes an EPUB a
KEPUB on the way, Calibre's quick KEPUB; and places the file as
`{author_sort}/{title} - {authors}.kepub.epub` unless changed. Cousoir copies
each file whole under a hidden name, then renames it, and deletes the Kobo's
cached covers of the books sent so it makes them again.

Without Calibre, Cousoir sends to a Kobo by itself, following Calibre's Kobo
driver and its settings when Calibre left some: an EPUB made a KEPUB by
Cousoir's own converter (see [Converting books](#converting-books)) with
the library's metadata and cover, the file at the driver's save template,
and the book written into metadata.calibre and driveinfo.calibre as Calibre
writes them, so that Calibre, when it connects later, knows the books
Cousoir sent. The golden test sends the same books both ways and has
Calibre read both Kobos. A PDF goes as it is, without the library's
metadata, which Calibre would write into it. A Kindle still needs Calibre.

What the Kobo knows of each book's reading shows too, read from its
database as Calibre's Kobo driver reads it to fetch annotations: the Kobo's
page has a Read column, how far each book is read or ✓ once finished, and a
Notes column; a book's page tells, for each reader that has it, how far it
is read, when it was last read and for how long, then its highlights, notes
and bookmarks by chapter, how far into the chapter and when each was made.
Copy puts them on the clipboard as Markdown, a quote for each highlight with
its note below; Save writes them to a Markdown file. Cousoir keeps what a
Kobo told last in `~/.local/share/cousoir/reading/<library>.json`, so a
book's page still shows it once the Kobo is unplugged, saying when it was
seen. Neither the library nor the Kobo is written for it.

Cousoir departs from Calibre here:

- It never writes to the Kobo's own database, `.kobo/KoboReader.sqlite`,
  which it only reads. The Kobo takes in the books sent, and forgets the
  books removed, once unplugged. Calibre also edits the database: it puts its
  series and collections there, clears the record of a book deleted on the
  Kobo before sending it again, and records a new file's size so that the
  Kobo keeps the reading position of a book replaced. A book sent again with
  Cousoir may start over on the Kobo.
- Plugging a Kobo in changes nothing on it. Calibre writes
  `driveinfo.calibre` and `metadata.calibre` as soon as it connects, and
  removes empty folders at the Kobo's root; Cousoir writes them when it
  sends.
- Removing books, Cousoir takes their entries out of `metadata.calibre` and
  leaves the others as they are; Calibre writes them all again from the
  Kobo's database, which it does anyway whenever it connects.

### Kindle

A Kindle reached over MTP is opened for each thing Cousoir does there, then
let go: MTP lets one program at a time have the device, and Calibre or a file
manager can have it in between. It needs no ejecting. Calibre's MTP driver
sends AZW3, then MOBI, AZW and PDF; places the file in `documents` as
`{title} - {authors}.azw3`; and puts with it its page numbers, an APNX file
in its `.sdr` folder, and its cover in `system/thumbnails`, with a copy in
`amazon-cover-bug`. Once a Kindle syncs with Amazon, it replaces the covers
of books not bought from Amazon with blank ones; as Calibre does whenever it
connects, Cousoir puts them back from those copies when it reads the Kindle.
Removing a book removes its file, its cover and the folders it leaves empty.
The Calibre job runs the MTP driver on a stand-in for the Kindle, its files
as Cousoir listed them, and notes what the driver would do to them, which
Cousoir then does over the connection it kept open from the listing on.

Without Calibre, Cousoir sends to a Kindle by itself, as Calibre's MTP
driver does with its settings: the library's metadata written into the
AZW3 or MOBI copy as Calibre's MOBI metadata writer writes it (the EXTH
records, the title, the cover and its thumbnail in the book where they
fit), the file in `documents` at the driver's send template, its cover
thumbnail in `system/thumbnails` with its copy against Amazon's bug, its
APNX page numbers by the fast method, and the book in `metadata.calibre`
and `driveinfo.calibre`. The golden test sends the same books both ways:
the books are Calibre's byte for byte, but for the cover images, the
page numbers too, and Calibre, connecting, knows the books Cousoir sent. A
PDF goes as it is, without the library's metadata. A book without a cover
gets no thumbnail, where Calibre puts its default cover. Page numbers are
always counted the fast way: Calibre's other methods, and a page count
from a column, need Calibre.

The highlights, notes and bookmarks made on the Kindle show as the Kobo's
do, read from `documents/My Clippings.txt`, which the Kindle writes in its
own language (English, French, German, Spanish, Italian, Portuguese and
Dutch dates are read): on a book's page, each with its page and location
and the day it was made, copied or saved as Markdown, and kept once the
Kindle is unplugged. A highlight changed on the Kindle leaves its old
version in the file: the last one shows. A note shows under the highlight
it was made on. The books are matched with the library's by title and
authors, as Calibre matches a reader's books, including books no longer
on the Kindle, whose clippings stay. The Kindle writes there neither how
far a book is read nor for how long. Calibre reads no clippings; the file
shows among the Kindle's books, as in Calibre.

Cousoir departs from Calibre here:

- The books Calibre has not seen yet, bought from Amazon or copied by hand,
  show by their file names. Calibre reads their metadata from their files
  when it connects, and adds them to `metadata.calibre`; Cousoir adds only
  the books it sends.
- Calibre notes each MTP device it connects to in its settings,
  `mtp_devices.json`; Cousoir does not.
- Older Kindles that come as a disk rather than over MTP are not handled.
- Writing metadata into a MOBI file without an EXTH header, Calibre loses
  its text: the second rewrite reads the file at its first length. Cousoir
  keeps it.

## Sharing on the network

Settings, Network, Share the library serves the library open to the other
devices of the local network, read only, while Cousoir is open:

- an OPDS catalog, at `http://ADDRESS:8090/opds`, which KOReader, Thorium,
  Moon+ Reader, PocketBook's and other readers browse: the newest books,
  every book by title, the authors, series, tags, publishers and languages,
  each leading to its books, and a search in Calibre's search language
  (`tags:"=Fantasy" and not series:true`). Each book shows its cover, its
  authors, description, rating, tags and series as Calibre's content server
  describes them, and a download link for each of its formats;
- a web page, at `http://ADDRESS:8090/`, for a phone's or a computer's
  browser: a search box, then the books' covers, titles and authors, each
  format a link to download it, the newest first.

The Network section lists the addresses, one per network the computer is
on, each with a QR code that a phone's camera opens and a Copy button; Look
again finds them after joining another network. The port is 8090 unless
changed, so that Calibre's content server, on 8080, can run beside it. A
password, when set, is asked for by every reader and browser (any user name
goes); without one, anyone on the network can download the books. The
setting is remembered: Cousoir shares the library again when it opens, and
the status bar shows it, a click away from the Network section, whenever
the library is shared.
Files are named as Calibre's content server names them, `Title - Authors_ID.epub`.

`cousoir-cli serve` shares a library the same way without the application,
on a server or a NAS for instance; it reads the library again when Calibre
changes it.

Cousoir departs from Calibre's content server here: it reads and nothing
else (no editing, no reading in the browser, no users with their own
rights), serves the one library open, and speaks HTTP only, for the local
network: to reach it from elsewhere, put it behind a proxy with HTTPS at
the root of a host name, as its links start at `/`.

## Settings

Settings has four sections. Appearance picks the theme and the text size, as
in JolieBulle. Library shows the library folder, lets you pick another one
(Cousoir remembers it) or go back to the library Calibre opens; says whether
the library can be written now, on which rules, and where the backups go;
says whether other computers share the library, as found or as set there
(Automatic, Shared, Only this computer); sets Calibre's interface language,
which decides title sorts, the "Unknown" of a book without author and how
names in other scripts become folder names, when Calibre runs in another
language on the computers sharing the library; and lists the conflict copies
found beside `metadata.db`: Dropbox's, Syncthing's, Nextcloud's and
ownCloud's. A conflict copy of the database keeps the library read-only until
you have checked in Calibre that no change is missing and pressed Ignore;
another copy, or this one changed, counts again. Network shares the library
on the local network (see [Sharing on the network](#sharing-on-the-network)).
About shows Cousoir's version, its credits, Calibre's first, and
its license.

## The library

By default Cousoir opens the library Calibre opens on this computer, read from
Calibre's `global.py.json` (`CALIBRE_CONFIG_DIRECTORY` is honoured). When
Calibre has none yet, the books page welcomes you and asks for a library's
folder; Reload reads Calibre's choice again, so a library just made in Calibre
opens without restarting. Set `COUSOIR_LIBRARY_PATH` to open another one:

```bash
COUSOIR_LIBRARY_PATH=tests/data/library ./build/cousoir
```

Cousoir never opens `metadata.db` in place. It copies the file to
`~/.cache/cousoir/<library>/snapshot.db`, retrying while Calibre's journal is
there or the file changes during the copy, and reads the copy with SQLite's
immutable flag. Nothing is written next to the database, so Dropbox has
nothing to sync and Calibre nothing to notice. Cover thumbnails are kept in
the same cache folder. A change to the database, from Calibre on this
computer or from Dropbox, reloads the library after a moment; F5 or Ctrl+R
reloads it at once. A reload that fails while Calibre writes keeps the books
shown and says so in the status bar.

The connection is set up as Calibre's: its SQL functions (`title_sort`,
`author_to_author_sort`, `uuid4`, `books_list_filter` and the legacy
aggregates), its temporary author triggers, foreign keys and a 10 s busy
timeout. `title_sort` follows Calibre's interface language and tweaks on this
computer, as Calibre's triggers do.

The database must have Calibre's schema version 28 and a structure Cousoir
knows: a fingerprint of its tables, columns, indexes, triggers and views,
custom columns left out. Libraries created by Calibre 9.16 and libraries
created years ago and upgraded since have the same one. Any other structure
keeps the library read-only.

Cousoir finds each book's cover and files the way `calibredb list` does,
including a file whose name differs from the one Calibre expects. Calibre
renames such a file when it finds it; Cousoir only reads it.

## cousoir-cli

```bash
cousoir-cli list [--library PATH]
cousoir-cli search [--library PATH] [--limit N] EXPRESSION…
cousoir-cli doctor [--library PATH]
cousoir-cli apply [--library PATH] [--force] changes.json
cousoir-cli read FILE…
cousoir-cli device [--library PATH] KOBO|mtp|mtp:SERIAL
cousoir-cli template [--library PATH] templates.json
cousoir-cli download [--library PATH] [--covers] BOOK_ID
cousoir-cli kepubify BOOK.epub BOOK.kepub
cousoir-cli serve [--library PATH] [--port 8090] [--password PASSWORD]
```

`serve` shares the library on the local network until stopped, as the
application's Network settings do: it prints its addresses, then each
request answered, and reads the library again when Calibre changes it.

`list` prints the books exactly as `calibredb list --for-machine --fields
all` does, byte for byte. `search` prints the ids of the books an
expression finds, comma-separated, as `calibredb search` does, and fails
the same way when it finds none or cannot read the expression. `doctor` reports the library's books and files,
the problems it finds (a file or folder the database lists but the disk does
not have, a cover Calibre does not list, a conflict copy, an unknown
structure, and what the Check section finds, but what was ignored there),
how the library is shared with other computers and the checks that would
keep it read-only. It exits with 1 when it finds a problem. Both read a copy of the database in a temporary folder.

`apply` saves the edits of a JSON file as the application saves them, with
the same checks unless `--force` skips them, all but Calibre's lock, which
the save still takes, and leaves the same heartbeat after a change:

```json
[{ "book": 1, "set": { "title": "Mood Indigo", "tags": ["Roman", "Jazz"], "coverFile": "cover.png" } }]
```

The keys are the details pane's: `title`, `sort`, `authors`, `authorSort`,
`series`, `seriesIndex`, `tags`, `rating` (0 to 10), `publisher`, `languages`,
`identifiers`, `pubdate`, `comments`, `coverFile` (relative to the JSON file)
and `removeCover`, and a custom column by its search term, `"#genre":
"SF, Classique"`, with `"#saga_index"` for a series' number. The other changes, after the edits and in this order,
the formats a book has no file for copied to it from another first:

```json
[{ "copyFormats": { "from": 7, "to": 2 } },
 { "removeFormats": { "book": 2, "formats": ["AZW3"] } },
 { "trash": [4] },
 { "restore": 4 }, { "restore": { "book": 2, "format": "AZW3" } },
 { "add": "book.epub" }, { "add": ["book.epub", "book.pdf"] },
 { "addFormat": { "book": 1, "file": "book.mobi" } }]
```

An `add` makes one book of its files, with the first one's metadata; files
are relative to the JSON file. A `bulk` edits several books together, as
the pane beside the list does, after the edits and before the other
changes, each one on the books as the changes before it left them:

```json
[{ "bulk": { "books": [1, 4], "addTags": ["Lu"], "removeTags": "À lire",
             "series": "Discworld", "numberSeries": true, "seriesStart": 1 } }]
```

Its keys are `books`, `authors`, `rating`, `publisher`, `series`,
`numberSeries`, `seriesStart`, `seriesIncrement`, `languages`, `pubdate`,
`addTags`, `removeTags` and `removeAllTags`. Conversions run first, on the books as the
library has them, and their files are added with the formats added:

```json
[{ "convert": { "book": 1, "format": "KEPUB" } }]
```

Books sent to a reader, or removed from it, come last, once the library's
changes are saved: a Kobo by the folder it is mounted on, a Kindle by
`mtp`, the first one connected, or `mtp:SERIAL`; removed books are named by
their path on the reader:

```json
[{ "send": { "books": [1, 2], "device": "/run/media/me/KOBOeReader" } },
 { "removeFromDevice": { "device": "/run/media/me/KOBOeReader",
                         "lpaths": ["Vian, Boris/Ecume des jours, L_ - Boris Vian.kepub.epub"] } }]
```

Calibre's log of a conversion or a sending is in its job's folder, in a
temporary folder removed at the end unless `COUSOIR_KEEP_JOBS` is set.

`template` prints, as JSON, what each template of a JSON array makes of
each book of the library, by book id, or `{"error": message}`, as
`tools/oracle/template_oracle.py` prints Calibre's.

`download` asks Calibre's metadata sources, on the web, about a book and
prints, as JSON, the books they found, each with the changes it would make,
the fields Calibre is set to leave aside left out; `--covers` adds the
covers found with the first. `apply` takes what it found back:

```json
[{ "download": { "book": 2, "found": "folder", "result": 0,
                 "fields": ["title", "tags", "pubdate"], "append": false } }]
```

with `found` the folder of an identify job's `result.json`, relative to the
JSON file, and the fields among `title`, `authors`, `rating`, `publisher`,
`tags`, `identifiers`, `pubdate`, `series`, `languages` and `comments`;
`append` adds the description found to the book's own.

`device` prints, as JSON, the books on a reader, named as above, the
library book each one is and, for a Kobo, what it knows of their reading,
as `tools/oracle/kobo_reading.py` prints Calibre's, without changing
anything. For the tests, a
reader `mtpdir:FOLDER` is a folder that stands for a Kindle over MTP.

`read` prints, as JSON, the metadata adding each file would read, custom
columns included, as `tools/oracle/read_oracle.py` prints Calibre's.

## Keyboard

| Shortcut | Action |
|---|---|
| Up/Down, arrows in the grid | Move in the list |
| Page Up, Page Down, Home, End | Move by a page, or to the first or last book |
| Ctrl+click, Shift+click, Shift with the moving keys | Select several books |
| Ctrl+A | Select every book listed |
| Esc | Back to one book, from several selected |
| Enter, Space, double click | Open the book, or the section's item |
| Menu, Shift+F10, right click | Book actions: open, open in reader, show folder, convert, send, move to trash |
| Ctrl+N | Add books |
| Ctrl+Shift+K, Ctrl+Shift+A | Convert the book, or the selected one, to KEPUB or AZW3 |
| Ctrl+D | Send the book, or the selected one, to the reader plugged in |
| Del | Move the selected books to the trash, or remove a book from the reader |
| Ctrl+O | Open the book, or the selected one, in the reader |
| Ctrl+G | Show the books as covers or as a table |
| Esc, Alt+Left | Back up one level |
| Ctrl+F | Search |
| Ctrl+K | Find a book, author, series, tag, publisher, section or action by a few letters |
| F5, Ctrl+R | Reload the library, or read the reader again |
| Ctrl+I | Show or hide the book details pane, or the pane editing several books |
| Enter, Tab, Esc | Save, save and move on, or revert a field |
| Ctrl+V | Paste the clipboard's image as the cover |
| Ctrl+Enter, Esc | Save the description or put it back, while editing it |
| Enter, Esc | Revert the book's changes or keep them, once asked |
| Enter | Restore the selected entry of the trash |
| Enter, Menu, Shift+F10 | A finding's actions, in the check |
| Ctrl++, Ctrl+-, Ctrl+0 | Text size |

## Requirements

- Qt 6.9 or later with QML, Quick, Quick Controls 2, SVG, Network, Xml,
  Concurrent and, on Linux, D-Bus
- SQLite 3.31 or later
- libzip
- For adding PDF files, Calibre or poppler's `pdfinfo` and `pdftoppm`; for
  formats other than EPUB, KEPUB, MOBI, AZW, AZW3, PRC and PDF, Calibre
- For converting and sending to a reader, Calibre
- For ejecting a Kobo, UDisks2 and Qt D-Bus
- For a Kindle over MTP, libmtp
- CMake 3.21 or later
- A C++20 compiler

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(tools/build_jobs.sh)"
./build/cousoir
```

Give `--parallel` a number. A build runs about 100 compilers taking up to
1 GB each, and without a number `make` runs them all at once and runs out of
memory. `tools/build_jobs.sh` gives as many as the cores and the free memory
allow.

`make build`, `make run`, `make test` and `make install` (under `~/.local`)
are available as shortcuts. They pick the number the same way, and
`make JOBS=4` sets it.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

`tools/sandbox.sh` runs a command with its home, temporary, cache, data and
config folders, and Calibre's, under `build/sandbox`: the tests then write
nothing outside the source tree, not even Qt's `~/.qttest`.

```bash
tools/sandbox.sh ctest --test-dir build --output-on-failure
```

Besides what builds Cousoir, the tests need only Calibre, for the parts that
run it. `cousoir-test-tool`, built with them, does the rest: it compares what
Calibre and Cousoir wrote, stands in for the readers and for Calibre's
sending job, and writes the book files of the fixtures.

The tests check the port against Calibre itself:

- `calibre_text_tests` and `calibre_paths_tests` compare the title and author
  sorts and the folder and file names with what Calibre 9.16 computes for the
  same names, in `tests/data/oracle/`, written by `make oracle`, which also
  generates `calibre_unidecode.inc` and `calibre_strings.inc`, the
  transliteration table and Calibre's translations Cousoir uses.
- `calibre_search_tests` checks the palette's search, and that the search's
  folds agree with Calibre's
  collators on every pair of `tests/data/oracle/search.json`, written by
  `make oracle`, which also generates `calibre_search.inc`, the folds; then
  parses and runs searches on the fixture, with saved searches, virtual
  libraries, grouped search terms and user categories.
- `description_tests` checks the description's text both ways: HTML read as
  text, text saved as HTML, and a description saved from its text reading
  back as the same text.
- `opf_tests` checks that Cousoir writes the `metadata.opf` files of the
  fixture libraries exactly as Calibre wrote them, custom columns included,
  and reads the custom columns of OPF files of both versions.
- `commit_engine_tests` saves edits to copies of the fixture: folders moving,
  case changes, cleaned values, dates, covers, the refusals, a change undone
  part way, a change started again over a concurrent one, and the recovery of
  a change interrupted before or after the database was replaced; and books
  added, moved to the trash and restored, a failing change of them undone
  whole, formats copied from a book before it goes to the trash, and a file
  copied in before a crash removed on recovery; and the values custom
  columns give new books.
- `library_check_tests` checks the check: name variants and initials, the
  names it must keep apart, duplicates by title and by ISBN, missing files and
  the files outside a copy of the fixture with files planted in it, and the
  changes its fixes make.
- The template language's tables, the built-in fields and Calibre's names
  of days and months in every language, are `calibre_template.inc`, written
  by `make oracle`.
- `tests/data/import` holds book files for the corners of Calibre's readers:
  EPUB package files of both versions, MOBI and AZW3 files made by
  `ebook-convert`, PDF files with an info dictionary and an XMP packet (one
  written by Calibre), covers to draw, metadata only in a file's name, and an
  EPUB of each version and a PDF exported by Calibre from a library with
  custom columns. `tools/make_import_fixtures.sh` builds them again.
- `tests/data/library` is a six-book library made by `calibredb`, with
  `tests/data/library-list.json`, its `calibredb list` output; `make fixtures`
  (`tools/make_fixture_library.sh`) builds both again.
  `tests/data/library-custom` is the same with a custom column of each kind
  Calibre has, filled in, and `tests/data/library-custom-list.json`
  (`tools/make_custom_library.sh`). `calibre_db_tests`
  reads it, checks Cousoir's list against Calibre's byte for byte, and checks
  that reading leaves every file of the library as it was.
- `golden_list` runs `calibredb list` and `cousoir-cli list` on separate
  copies of the fixture and compares them, and on 2000 made-up books from
  `cousoir-test-tool make-library`, below. On both, with saved searches,
  virtual libraries, groups and categories added
  (`tests/data/search/preferences.sql`), it runs the searches of
  `tests/data/search/queries.txt` through Calibre's search
  (`tools/oracle/search_books.py`) and Cousoir's, which must find the same
  books or both refuse the search. It runs the templates of
  `tests/data/templates/templates.json` through Calibre's formatter
  (`tools/oracle/template_oracle.py`) and `cousoir-cli template` on the
  same three libraries, which must make the same text of each book, or
  both fail. It sorts them by the sort histories of
  `tests/data/sort/sorts.json` with Calibre's `multisort()`
  (`tools/oracle/sort_oracle.py`) and `cousoir-test-tool sort-books`, which
  must give the same order. With `COUSOIR_GOLDEN_REAL=1` it
  does the same with the library Calibre opens. It then applies the edits of
  `tests/data/edits/edits.json`, and apart the bulk edits of
  `tests/data/edits/bulk.json` and the custom columns' edits of
  `tests/data/edits/custom.json` on `library-custom`, with Calibre's own writers and bulk edit
  (`tools/oracle/apply_edits.py`) and with `cousoir-cli apply`, and compares
  every table and every file, `metadata.opf` files as XML and covers by their
  pixels; `calibredb check_library` must report both copies alike. Finally
  Calibre writes again the `metadata.opf` files Cousoir wrote, and must find
  nothing to change, then edits the copy, which Cousoir must read as calibredb
  lists it. It also compares `cousoir-cli read` with Calibre's readers
  (`tools/oracle/read_oracle.py`) on `tests/data/import` and the fixture's
  files, and on every file of the real library with
  `COUSOIR_GOLDEN_REAL=1`. It runs Cousoir's identify job under Calibre
  with `single_identify()` answering the books of
  `tests/data/download/found.json`, then applies the downloads of
  `tests/data/download/download.json` through Calibre's Edit metadata
  dialog, shown offscreen (`tools/oracle/download_oracle.py`), and through
  `cousoir-cli apply`, and compares the two libraries. Then it adds,
  removes and restores books and
  formats, and keeps one copy of a book with another's formats, in four steps
  (`tests/data/edits/ops-*.json`), each compared table
  by table and file by file, the trash included, with page counts and the
  time and uuid of the books added left out; and has Calibre restore what
  Cousoir moved to the trash and Cousoir what Calibre did, each ending as the
  other. The same on `library-custom`, its columns giving new books values
  (`tests/data/edits/defaults.sql`), adds the books Calibre exported with
  custom columns and restores books with theirs
  (`tests/data/edits/ops-custom-*.json`); the oracle reads the files to add
  before it opens the library, as Calibre's Add books does. Composite
  columns' values are left out of the comparison. Last, it converts books of the fixture with Calibre's convert action
  (`tools/oracle/convert_books.py`) and with `cousoir-cli apply`
  (`tests/data/edits/convert.json`), some with settings saved by earlier
  conversions, one to its own format; with `COUSOIR_GOLDEN_REAL=1`, books of
  the real library too, with the user's Calibre settings. The converted
  files compare by content: an EPUB or KEPUB by its entries, an AZW3 by what
  Calibre explodes it to, without the time stamped in them, the random ids
  Calibre gives the anchors it adds, and the order of an AZW3's stylesheets
  and images and of an element's classes, which change from one Calibre
  process to the next. Calibre draws a book's missing cover in a random
  style: the tests leave it one. Then it sends books of the fixture to fake
  readers, with Calibre's GUI (`tools/oracle/send_books.py`) and with
  `cousoir-cli apply`: Kobos, a folder each with a `.kobo/KoboReader.sqlite`
  made from `tests/data/kobo.sql`, and Kindles, a folder each standing for
  one over MTP, which Calibre's MTP driver reaches with libmtp's calls done
  on the folder. Some books go, then more once the reader had them a while
  (`cousoir-test-tool kobo-import` does what a Kobo does once unplugged,
  `kindle-sync` what Amazon does to a Kindle's covers), one
  of them again, then one is removed. Each step compares the readers' files,
  the books by content and `metadata.calibre` and `driveinfo.calibre` as
  JSON, without the times, random ids and temporary paths in them
  (`cousoir-test-tool compare-devices`); then Calibre connects to both, which
  must end alike, and each program must recognise the books the other sent.
  With `COUSOIR_GOLDEN_REAL=1`, books of the real library go too. Last, it
  reads a fake Kobo whose database has books being read, finished and
  bought, with highlights, notes and bookmarks
  (`tests/data/kobo-reading.sql`), with Calibre's Kobo `Bookmark`
  (`tools/oracle/kobo_reading.py`) and with `cousoir-cli device`, which
  must find the same reading, notes, chapters and order. It is skipped
  without Calibre.
- `test_tool_tests` checks that the comparisons `golden_list` relies on find
  what differs, a row, a file, a package file, a cover, a converted book, a
  book on a reader or a field read, and let through what may: attributes in
  another order, a cover encoded again, the time and ids of a conversion.
- `conversion_tests` checks the OPF a conversion reads against Calibre's
  (`tests/data/oracle/conversion.json`, from `make oracle`), the job's files,
  how its outcome is read and added, and, with Calibre installed, converts a
  book of the fixture to KEPUB. `book_library_tests` and the UI tests drive
  conversions with `tests/data/fake-calibre-debug` standing in for Calibre.
- `kobo_tests` reads fake Kobos: the model from a fake `/sys`, the books from
  their database and `metadata.calibre`, their reading and notes, the log
  kept of them, Calibre's matching with the library,
  the cover cache Calibre computes, removals that leave the other entries of
  `metadata.calibre` as written; and the sending jobs, their outcome and how
  their files reach the Kobo, and, with Calibre installed, sends a book of
  the fixture. `kindle_tests` does the same for Kindles, on folders that
  stand for them: Calibre's rules for the folders it ignores, the books,
  removals with their covers, the covers put back, the jobs and how their
  operations reach the Kindle, and "My Clippings.txt" in three of the
  Kindle's languages (`tests/data/kindle`). `book_library_tests` and the UI tests drive
  readers with fake lists of mounted volumes and Kindles and
  `cousoir-test-tool` standing in for Calibre; they never see the
  readers plugged into the computer, which can stay plugged in.
- `concurrency_guard_tests` drives the write checks with a fake
  `/proc/net/unix`, a fake Dropbox socket, Syncthing and Nextcloud folders
  made in a temporary folder and a file system type given in place of the
  real one.
- `library_server_tests` serves the fixture library on a port of
  127.0.0.1 and reads it as a reader would: the catalog's feeds followed
  link by link, searches, categories, files, covers and thumbnails, the
  password, the web page with what was searched escaped, and long feeds in
  pages. `qr_code_tests` compares Cousoir's QR codes, module by module,
  with libqrencode's (`tools/make_qr_fixtures.sh` made them), and reads
  them back with `zbarimg` when it is installed.
- `mobi_metadata_tests` writes metadata into MOBI and AZW3 books, Calibre's
  conversions and two it never makes, and compares them byte for byte with
  what Calibre's own writer made of them (`tools/oracle/mobi_oracle.py`),
  the cover images aside; the thumbnail names and APNX files too.
- `metadata_web_tests` checks the Open Library source: Calibre's search
  words (`tests/data/oracle/source.json`), the search and work answers Open
  Library once gave, read; dates; Calibre's ranking; and, against a fake
  Open Library on 127.0.0.1, a search by ISBN, its fallback to the title,
  covers, and a site that cannot be reached.
- The UI tests run offscreen with the software Quick backend at 100% and 200%
  scale, against a copy of the fixture library; the share listens on
  127.0.0.1 only (`COUSOIR_SHARE_LOCALHOST`).

### Measuring

`cousoir-test-tool make-library FOLDER BOOKS [SEED]` writes a library of
made-up books, the same ones for the same seed, through Calibre's own schema
and triggers: authors, series, tags and publishers of which a few fill most
books, as in a real library, titles in several languages, names in other
scripts, the same book now and then, files and covers. `cousoir-benchmark`
then measures what a user waits for, on a copy, as it saves an edit:
loading, checking, searching letter by letter, sorting, browsing, opening a
book and saving; with `--ui`, the whole window, each step until its frame is
drawn.

```bash
build/cousoir-test-tool make-library build/sandbox/libs/50k 50000
cp -a --reflink=auto build/sandbox/libs/50k build/sandbox/libs/bench
sync -f build/sandbox
QT_QPA_PLATFORM=offscreen tools/sandbox.sh build/cousoir-benchmark --ui build/sandbox/libs/bench
```

The `sync` first: a save ends by flushing the library's folder to the disk,
which on btrfs also writes whatever the copy left pending.

## License

GPL-3.0-only, as Cousoir holds code ported from Calibre, which is GPL v3; see
`LICENSE`, and [CREDITS.md](CREDITS.md) for what comes from Calibre. The theme
files taken from kbextract, JolieBulle and omadji (`Theme.qml`,
`omarchy_theme.*`, `system_appearance.*`, `AppearanceSettingsPage.qml`,
`SettingsWindow.qml` and the shared controls) also keep their MIT notice, in
`LICENSES/MIT.txt`, and the icons adapted from Lucide their ISC notice, in
`LICENSES/ISC-lucide.txt`.
