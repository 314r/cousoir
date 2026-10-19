# Reliure

A Qt/QML ebook manager that works on a Calibre library: the books, sorted and
searched as a table or as covers, the authors, series, tags, languages and
publishers that browse them, and each book's metadata, description and files,
which open in the desktop's reader. A book's metadata and cover edit in
place; books and files are added, moved to the library's trash and restored
from it; Calibre converts books to KEPUB for a Kobo and AZW3 for a Kindle;
books go to a Kobo plugged in, and come off it.

Calibre stays the library's owner and keeps working as before. Reliure reads
and writes Calibre's own `metadata.db` and follows Calibre's rules wherever it
computes something Calibre computes, so both always agree; the tests compare
its writes with Calibre's own. Sending to a Kindle comes in the next version.

The project structure, theming (system, light, dark and Omarchy) and Settings
window come from the JolieBulle Qt port, which took them from kbextract.

## Interface

The interface follows the Omarchy style: the system monospace font, square
corners, hairline dividers, foreground-tinted hover and selection states
instead of accent fills, and one accent per screen.

The sidebar has the books and the trash, then the browse sections with their
sizes: authors, series, tags, languages and publishers, then the Kobos
plugged in. A section lists its items
with their number of books; Enter or a double click lists an item's books,
with the item in the breadcrumb. Esc, Alt+Left or the back button goes back
up one level.

The books show as a table or, with Ctrl+G or the toolbar button, as a grid of
covers; both keep the same book selected. Click the Title, Authors, Series,
Added or Rating column header to sort, and click it again to reverse the
order. Titles sort by Calibre's title sort, "Petit Prince, Le", and authors by
their author sort, as Calibre sorts them. Columns leave a narrow window in a
set order, Formats first. The search, Ctrl+F, finds every word typed in a
book's title, authors, series or tags, with or without accents.

A book opens with Enter, Space or a double click. Its page shows the cover,
the title, authors, series and rating, and the description, beside a details
pane with the rest of its metadata, its identifiers (some link to their page)
and its files. Below 960 px of window width the pane hides; the status bar
toggle (Ctrl+I) shows it anyway. A file's button, or Open in reader (Ctrl+O),
opens it with the desktop's application for that format; Open in reader picks
EPUB, KEPUB, AZW3, MOBI or PDF, in that order. Ctrl+O also works on the
selected row of a list. A row's menu (right click, Menu or Shift+F10) opens
the book, opens it in the reader, shows its folder, converts it, sends it to
a Kobo or moves it to the trash.

When the library cannot be written right now, the status bar says why with a
lock: Calibre open on this computer, Calibre writing, Dropbox syncing or not
running, Reliure editing on another computer, a change from elsewhere in the
last two minutes, or a Dropbox conflict copy of the database. A click on the
lock opens the library settings.

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

Once the open book has changed, Revert changes in the toolbar puts it back as
it was when you opened it, cover included, after asking (Enter reverts, Esc
keeps). Closing the book makes its changes final.

### How a change is saved

Each change is saved at once, one after the other, on a worker thread:

1. Reliure takes Calibre's own lock on its libraries, so neither the Calibre
   GUI nor calibredb can start on this computer meanwhile.
2. It reads `metadata.db`, keeping a copy of it before the session's first
   change in `~/.local/share/reliure/backups/<library>/` (the last 30).
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

A change that sets values the book already has writes nothing. Reliure's own
change does not start the two-minute wait, and Dropbox sending it does not
make the library read-only. After a change, Reliure leaves a heartbeat in a
`.reliure` folder at the Dropbox root, outside the library: for ten minutes,
the Reliure of another computer sharing the library stays read-only.

Reliure departs from Calibre on purpose in a few places:

- An item with a note in `.calnotes` stays when no book uses it any more;
  Calibre removes it and retires the note.
- When a book's folder would move onto a folder that is not empty, Reliure
  stops; Calibre would delete that folder.
- A database whose authors, tags, series or publishers differ only by case is
  left for Calibre to tidy: Reliure refuses to write those fields.
- With Calibre in Japanese, Korean or Vietnamese, whose folder names come
  from other tables, Reliure does not write.
- The description is not editable yet.

## Adding and removing books

Ctrl+N, or the + button of the toolbar, picks book files to add; files dropped
on the book list are added too, and a folder dropped there adds the book files
in it, those with an extension Calibre takes for a book. Each file becomes a
book. Reliure reads the files on a worker thread, the status bar counting
them, and reads what `calibredb add` reads:

- the title and authors in the file's name, by Calibre's file name pattern
  (`Title - Author.epub` by default, or the one set in Calibre's preferences);
- then the file's own metadata, unless Calibre is set not to read it, merged
  as Calibre merges it: the file's title and authors unless they are
  "Unknown", its other fields where it has them, its tags with the name's, its
  cover. EPUB and KEPUB (OPF 2 and 3), MOBI, AZW, AZW3 and PRC are read by
  Reliure itself; PDF through poppler's `pdfinfo` and `pdftoppm`, Calibre's
  own copy when Calibre is installed, its XMP packet merged with its info
  dictionary as Calibre merges them; any other format, and the cover of an
  EPUB whose first page must be drawn, through Calibre itself when it is
  installed.

When books are already in the library by title and authors, as Calibre
compares them (lower case, without punctuation or a leading article, every
author found, the languages alike if both have some), Reliure asks whether to
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
files back into their book. Reliure restores what Calibre moved to the trash
and Calibre restores what Reliure did.

Reliure departs from Calibre here too:

- It never empties the trash. Calibre deletes entries older than 14 days, or
  its library's own setting, when it starts.
- It does not restore a book in a library with custom columns, whose values
  it does not write yet, nor a book whose `metadata.opf` holds annotations;
  and it does not add books to a library whose custom columns give new books
  a value. Calibre does all three.
- Calibre's file type plugins do not run: an HTML file is added as HTML where
  Calibre would zip it with its images.
- An OPF file named like the added file, beside it, is not read; Calibre
  would take its metadata.
- An old MOBI file without an EXTH header keeps the title of its header,
  where Calibre would also look for metadata in its text.
- Page counts are left for Calibre, which counts the pages of the books
  Reliure added or changed the next time it opens the library.

## Converting books

The Convert to buttons of the Files section of a book's details, the row menu
or Ctrl+Shift+K and Ctrl+Shift+A convert the book to KEPUB, for a Kobo, or
AZW3, for a Kindle; Reliure asks first when the book has a file of that
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
`ORIGINAL_AZW3` or `ORIGINAL_KEPUB`, as in Calibre. Converting needs Calibre;
without it the Convert actions do not show.

Reliure departs from Calibre here too:

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

## Sending to a Kobo

A Kobo plugged in and mounted shows in the sidebar under Devices, with the
name Calibre gave it or its model's. Its page lists the books on it: those
sent by Calibre or Reliure, those copied by hand and those bought from Kobo,
with their format, size and whether each is one of the library's, which Enter
opens. Del removes a book from the Kobo, after asking; bought books stay.
Eject unmounts the Kobo and powers it off, so it can be unplugged. The
status bar shows the room left on it.

The book's Files section has a Send to button for each Kobo plugged in, and
says which Kobos have the book; the row menu and Ctrl+D, for the first Kobo,
send too. Books go one batch after the other, each once the changes before
it are saved; the status bar and the Files section show how far it got, and
Stop in the Files section or the × of the status bar stops it.

Calibre sends, as its "Send to device" does with the settings of its Kobo
driver: the format the driver prefers that the book has, KEPUB, then EPUB,
then PDF and the others; the library's metadata and cover put into a copy of
the file; an EPUB made a KEPUB on the way, Calibre's quick KEPUB, with the
driver's settings; the file placed by the driver's save template,
`{author_sort}/{title} - {authors}.kepub.epub` unless changed; the book added
to `metadata.calibre`, which tells Calibre the books it sent and their
library uuid, and `driveinfo.calibre` updated. Calibre recognises the books
Reliure sent as the library's, and Reliure the books Calibre sent, by their
uuid, then by their title and authors as Calibre does. Calibre does the work
through calibre-debug, on copies of what it reads of the library and of the
Kobo; Reliure then copies the new files to the Kobo, each written whole under
a hidden name then renamed, the books first and `metadata.calibre` last, and
deletes the Kobo's cached covers of the books sent so it makes them again.
Sending needs Calibre; without it the Send buttons do not show. While Calibre
is open on this computer, it handles the Kobo: Reliure sends nothing and
removes nothing until it is closed.

Reliure departs from Calibre here:

- It never writes to the Kobo's own database, `.kobo/KoboReader.sqlite`,
  which it only reads. The Kobo takes in the books sent, and forgets the
  books removed, once unplugged. Calibre also edits the database: it puts its
  series and collections there, clears the record of a book deleted on the
  Kobo before sending it again, and records a new file's size so that the
  Kobo keeps the reading position of a book replaced. A book sent again with
  Reliure may start over on the Kobo.
- Plugging a Kobo in changes nothing on it. Calibre writes
  `driveinfo.calibre` and `metadata.calibre` as soon as it connects, and
  removes empty folders at the Kobo's root; Reliure writes them when it
  sends.
- Removing books, Reliure takes their entries out of `metadata.calibre` and
  leaves the others as they are; Calibre writes them all again from the
  Kobo's database, which it does anyway whenever it connects.
- Calibre offers to convert a book without a format the Kobo reads; Reliure
  says so, and the book can be converted to KEPUB first.
- There is no settings page: the Kobo driver's settings are Calibre's, set in
  Calibre.

## Settings

Settings has two sections. Appearance picks the theme and the text size, as
in JolieBulle. Library shows the library folder, lets you pick another one
(Reliure remembers it) or go back to the library Calibre opens; says whether
the library can be written now and where the backups go; sets Calibre's
interface language, which decides title sorts, the "Unknown" of a book without
author and how names in other scripts become folder names, when Calibre runs
in another language on the computers sharing the library; and lists the
Dropbox conflict copies found beside `metadata.db`. A conflict copy of the
database keeps the library read-only until you have checked in Calibre that
no change is missing and pressed Ignore; another copy, or this one changed,
counts again.

## The library

By default Reliure opens the library Calibre opens on this computer, read from
Calibre's `global.py.json` (`CALIBRE_CONFIG_DIRECTORY` is honoured). Set
`RELIURE_LIBRARY_PATH` to open another one:

```bash
RELIURE_LIBRARY_PATH=tests/data/library ./build/reliure
```

Reliure never opens `metadata.db` in place. It copies the file to
`~/.cache/reliure/<library>/snapshot.db`, retrying while Calibre's journal is
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

The database must have Calibre's schema version 28 and a structure Reliure
knows: a fingerprint of its tables, columns, indexes, triggers and views,
custom columns left out. Libraries created by Calibre 9.16 and libraries
created years ago and upgraded since have the same one. Any other structure
keeps the library read-only.

Reliure finds each book's cover and files the way `calibredb list` does,
including a file whose name differs from the one Calibre expects. Calibre
renames such a file when it finds it; Reliure only reads it.

## reliure-cli

```bash
reliure-cli list [--library PATH]
reliure-cli doctor [--library PATH]
reliure-cli apply [--library PATH] [--force] changes.json
reliure-cli read FILE…
reliure-cli device [--library PATH] KOBO
```

`list` prints the books exactly as `calibredb list --for-machine --fields
all` does, byte for byte. `doctor` reports the library's books and files,
the problems it finds (a file or folder the database lists but the disk does
not have, a cover Calibre does not list, a conflict copy, an unknown
structure) and the checks that would keep it read-only. It exits with 1 when
it finds a problem. Both read a copy of the database in a temporary folder.

`apply` saves the edits of a JSON file as the application saves them, with
the same checks unless `--force` skips the Dropbox and recent-change ones:

```json
[{ "book": 1, "set": { "title": "Mood Indigo", "tags": ["Roman", "Jazz"], "coverFile": "cover.png" } }]
```

The keys are the details pane's: `title`, `sort`, `authors`, `authorSort`,
`series`, `seriesIndex`, `tags`, `rating` (0 to 10), `publisher`, `languages`,
`identifiers`, `pubdate`, `comments`, `coverFile` (relative to the JSON file)
and `removeCover`. The other changes, after the edits and in this order:

```json
[{ "removeFormats": { "book": 2, "formats": ["AZW3"] } },
 { "trash": [4] },
 { "restore": 4 }, { "restore": { "book": 2, "format": "AZW3" } },
 { "add": "book.epub" }, { "add": ["book.epub", "book.pdf"] },
 { "addFormat": { "book": 1, "file": "book.mobi" } }]
```

An `add` makes one book of its files, with the first one's metadata; files
are relative to the JSON file. Conversions run first, on the books as the
library has them, and their files are added with the formats added:

```json
[{ "convert": { "book": 1, "format": "KEPUB" } }]
```

Books sent to a Kobo, or removed from it, by the folder it is mounted on,
come last, once the library's changes are saved; removed books are named by
their path on the Kobo:

```json
[{ "send": { "books": [1, 2], "device": "/run/media/me/KOBOeReader" } },
 { "removeFromDevice": { "device": "/run/media/me/KOBOeReader",
                         "lpaths": ["Vian, Boris/Ecume des jours, L_ - Boris Vian.kepub.epub"] } }]
```

Calibre's log of a conversion or a sending is in its job's folder, in a
temporary folder removed at the end unless `RELIURE_KEEP_JOBS` is set.

`device` prints, as JSON, the books on the Kobo mounted on the given folder
and the library book each one is, without changing anything.

`read` prints, as JSON, the metadata adding each file would read, as
`tools/oracle/read_oracle.py` prints Calibre's.

## Keyboard

| Shortcut | Action |
|---|---|
| Up/Down, arrows in the grid | Move in the list |
| Enter, Space, double click | Open the book, or the section's item |
| Menu, Shift+F10, right click | Book actions: open, open in reader, show folder, convert, send, move to trash |
| Ctrl+N | Add books |
| Ctrl+Shift+K, Ctrl+Shift+A | Convert the book, or the selected one, to KEPUB or AZW3 |
| Ctrl+D | Send the book, or the selected one, to the Kobo plugged in |
| Del | Move the selected book to the trash, or remove it from the Kobo |
| Ctrl+O | Open the book, or the selected one, in the reader |
| Ctrl+G | Show the books as covers or as a table |
| Esc, Alt+Left | Back up one level |
| Ctrl+F | Search |
| F5, Ctrl+R | Reload the library, or read the Kobo again |
| Ctrl+I | Show or hide the book details pane |
| Enter, Tab, Esc | Save, save and move on, or revert a field |
| Ctrl+V | Paste the clipboard's image as the cover |
| Enter, Esc | Revert the book's changes or keep them, once asked |
| Enter | Restore the selected entry of the trash |
| Ctrl++, Ctrl+-, Ctrl+0 | Text size |

## Requirements

- Qt 6.9 or later with QML, Quick, Quick Controls 2, SVG, Network, Xml,
  Concurrent and, on Linux, D-Bus
- SQLite 3.31 or later
- libzip
- For adding PDF files, Calibre or poppler's `pdfinfo` and `pdftoppm`; for
  formats other than EPUB, KEPUB, MOBI, AZW, AZW3, PRC and PDF, Calibre
- For converting and sending to a Kobo, Calibre
- For ejecting a Kobo, UDisks2 and Qt D-Bus
- CMake 3.21 or later
- A C++20 compiler

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/reliure
```

`make build`, `make run`, `make test` and `make install` (under `~/.local`)
are available as shortcuts.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

The tests check the port against Calibre itself:

- `calibre_text_tests` and `calibre_paths_tests` compare the title and author
  sorts and the folder and file names with what Calibre 9.16 computes for the
  same names, in `tests/data/oracle/`, written by `make oracle`, which also
  generates `calibre_unidecode.inc` and `calibre_strings.inc`, the
  transliteration table and Calibre's translations Reliure uses.
- `opf_tests` checks that Reliure writes the `metadata.opf` files of the
  fixture library exactly as Calibre wrote them.
- `commit_engine_tests` saves edits to copies of the fixture: folders moving,
  case changes, cleaned values, dates, covers, the refusals, a change undone
  part way, a change started again over a concurrent one, and the recovery of
  a change interrupted before or after the database was replaced; and books
  added, moved to the trash and restored, a failing change of them undone
  whole, and a file copied in before a crash removed on recovery.
- `tests/data/import` holds book files for the corners of Calibre's readers:
  EPUB package files of both versions, MOBI and AZW3 files made by
  `ebook-convert`, PDF files with an info dictionary and an XMP packet (one
  written by Calibre), covers to draw and metadata only in a file's name.
  `tools/make_import_fixtures.sh` builds them again.
- `tests/data/library` is a six-book library made by `calibredb`, with
  `tests/data/library-list.json`, its `calibredb list` output; `make fixtures`
  (`tools/make_fixture_library.sh`) builds both again. `calibre_db_tests`
  reads it, checks Reliure's list against Calibre's byte for byte, and checks
  that reading leaves every file of the library as it was.
- `golden_list` runs `calibredb list` and `reliure-cli list` on separate
  copies of the fixture and compares them; with `RELIURE_GOLDEN_REAL=1` it
  does the same with the library Calibre opens. It then applies the edits of
  `tests/data/edits/edits.json` with Calibre's own writers
  (`tools/oracle/apply_edits.py`) and with `reliure-cli apply`, and compares
  every table and every file, `metadata.opf` files as XML and covers by their
  pixels; `calibredb check_library` must report both copies alike. Finally
  Calibre writes again the `metadata.opf` files Reliure wrote, and must find
  nothing to change, then edits the copy, which Reliure must read as calibredb
  lists it. It also compares `reliure-cli read` with Calibre's readers
  (`tools/oracle/read_oracle.py`) on `tests/data/import` and the fixture's
  files, and on every file of the real library with
  `RELIURE_GOLDEN_REAL=1`. Then it adds, removes and restores books and
  formats in three steps (`tests/data/edits/ops-*.json`), each compared table
  by table and file by file, the trash included, with page counts and the
  time and uuid of the books added left out; and has Calibre restore what
  Reliure moved to the trash and Reliure what Calibre did, each ending as the
  other. Last, it converts books of the fixture with Calibre's convert action
  (`tools/oracle/convert_books.py`) and with `reliure-cli apply`
  (`tests/data/edits/convert.json`), some with settings saved by earlier
  conversions, one to its own format; with `RELIURE_GOLDEN_REAL=1`, books of
  the real library too, with the user's Calibre settings. The converted
  files compare by content: an EPUB or KEPUB by its entries, an AZW3 by what
  Calibre explodes it to, without the time stamped in them, the random ids
  Calibre gives the anchors it adds, and the order of an AZW3's stylesheets
  and images and of an element's classes, which change from one Calibre
  process to the next. Calibre draws a book's missing cover in a random
  style: the tests leave it one. Then it sends books of the fixture to fake
  Kobos, a folder each with a `.kobo/KoboReader.sqlite` made from
  `tests/data/kobo.sql`, with Calibre's GUI (`tools/oracle/send_books.py`)
  and with `reliure-cli apply`: four books, then two once the Kobo took the
  first ones in (`tests/golden/kobo_import.py` does what a Kobo does once
  unplugged), one of them again, then one removed. Each step compares the
  Kobos' files, the books by content and `metadata.calibre` and
  `driveinfo.calibre` as JSON, without the times and temporary paths in them
  (`tests/golden/compare_devices.py`); then Calibre connects to both, which
  must end alike, and each program must recognise the books the other sent.
  With `RELIURE_GOLDEN_REAL=1`, books of the real library go too. It is
  skipped without Calibre.
- `conversion_tests` checks the OPF a conversion reads against Calibre's
  (`tests/data/oracle/conversion.json`, from `make oracle`), the job's files,
  how its outcome is read and added, and, with Calibre installed, converts a
  book of the fixture to KEPUB. `book_library_tests` and the UI tests drive
  conversions with `tests/data/fake-calibre-debug` standing in for Calibre.
- `kobo_tests` reads fake Kobos: the model from a fake `/sys`, the books from
  their database and `metadata.calibre`, Calibre's matching with the library,
  the cover cache Calibre computes, removals that leave the other entries of
  `metadata.calibre` as written; and the sending jobs, their outcome and how
  their files reach the Kobo, and, with Calibre installed, sends a book of
  the fixture. `book_library_tests` and the UI tests drive Kobos with a fake
  list of mounted volumes and `tests/data/fake-calibre-send` standing in for
  Calibre.
- `concurrency_guard_tests` drives the write checks with a fake
  `/proc/net/unix` and a fake Dropbox socket.
- The UI tests run offscreen with the software Quick backend at 100% and 200%
  scale, against a copy of the fixture library.

## License

GPL-3.0-only, as Reliure ports Calibre's rules; see `LICENSE`. The theme files
taken from kbextract and JolieBulle (`Theme.qml`, `omarchy_theme.*`,
`system_appearance.*`, `AppearanceSettingsPage.qml`, `SettingsWindow.qml` and
the shared controls) also keep their MIT notice, in
`LICENSES/MIT-omarchy.txt`.
