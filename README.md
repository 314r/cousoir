# Reliure

A Qt/QML ebook manager that works on a Calibre library: the books, sorted and
searched as a table or as covers, the authors, series, tags, languages and
publishers that browse them, and each book's metadata, description and files,
which open in the desktop's reader. A book's metadata and cover edit in
place; books and files are added, moved to the library's trash and restored
from it.

Calibre stays the library's owner and keeps working as before. Reliure reads
and writes Calibre's own `metadata.db` and follows Calibre's rules wherever it
computes something Calibre computes, so both always agree; the tests compare
its writes with Calibre's own. Conversion and sending to a Kobo or a Kindle
come in the next versions.

The project structure, theming (system, light, dark and Omarchy) and Settings
window come from the JolieBulle Qt port, which took them from kbextract.

## Interface

The interface follows the Omarchy style: the system monospace font, square
corners, hairline dividers, foreground-tinted hover and selection states
instead of accent fills, and one accent per screen.

The sidebar has the books and the trash, then the browse sections with their
sizes: authors, series, tags, languages and publishers. A section lists its items
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
the book, opens it in the reader, shows its folder or moves it to the trash.

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
are relative to the JSON file.

`read` prints, as JSON, the metadata adding each file would read, as
`tools/oracle/read_oracle.py` prints Calibre's.

## Keyboard

| Shortcut | Action |
|---|---|
| Up/Down, arrows in the grid | Move in the list |
| Enter, Space, double click | Open the book, or the section's item |
| Menu, Shift+F10, right click | Book actions: open, open in reader, show folder, move to trash |
| Ctrl+N | Add books |
| Del | Move the selected book to the trash |
| Ctrl+O | Open the book, or the selected one, in the reader |
| Ctrl+G | Show the books as covers or as a table |
| Esc, Alt+Left | Back up one level |
| Ctrl+F | Search |
| F5, Ctrl+R | Reload the library |
| Ctrl+I | Show or hide the book details pane |
| Enter, Tab, Esc | Save, save and move on, or revert a field |
| Ctrl+V | Paste the clipboard's image as the cover |
| Enter, Esc | Revert the book's changes or keep them, once asked |
| Enter | Restore the selected entry of the trash |
| Ctrl++, Ctrl+-, Ctrl+0 | Text size |

## Requirements

- Qt 6.9 or later with QML, Quick, Quick Controls 2, SVG, Network, Xml and
  Concurrent
- SQLite 3.31 or later
- libzip
- For adding PDF files, Calibre or poppler's `pdfinfo` and `pdftoppm`; for
  formats other than EPUB, KEPUB, MOBI, AZW, AZW3, PRC and PDF, Calibre
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
  other. It is skipped without Calibre.
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
