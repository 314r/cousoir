# The experimental branch

Autonomous work on Cousoir, started on 2026-10-22, for review before any of
it goes to `master`. This file holds the goals, how the work is done, where
each goal stands, and a journal of what was done and why.

Nothing here is merged into `master`, pushed or published: the review decides.

## How the work is done

- `experimental` branches from `master` at 7664f54. Each goal is worked on
  a branch of its own, `exp-<name>`, merged into `experimental` with
  `--no-ff` once its tests pass, as `master` takes its phases.
- Nothing is written outside the source tree. `tools/sandbox.sh` runs the
  tests, Calibre and trial runs of Cousoir with their home, temporary,
  cache, data and config folders under `build/sandbox`. The library Calibre
  opens on this computer is never opened, so `COUSOIR_GOLDEN_REAL` is never
  set.
- Calibre stays the reference. A feature that computes what Calibre computes
  is ported from Calibre's source (9.16, fetched into
  `build/sandbox/calibre-src`) and tested against Calibre itself, as the
  rest of Cousoir is; CREDITS.md and the file headers name what is ported.
- The build stays bounded (`make build`), and the tests run one at a time.
- Every few goals: a full test run, a review of the new code for bugs and
  for what can be simpler, and the documentation (README, DOC.md) brought
  up to date.

## Goals

Each goal says what it is for and when it is done.

### 1. A large library stays instant

Cousoir is "fast", but its books reach QML as one list of maps rebuilt on
every keystroke, sort and save, and the views reset each time. Nothing has
measured a library of tens of thousands of books.

- `cousoir-test-tool make-library` writes a large, varied library,
  deterministically, through Calibre's own schema and triggers.
- A benchmark measures loading, searching, sorting, opening a book, saving
  an edit and checking the library, at 1 000, 10 000 and 50 000 books.
- What the measures show slow is fixed. Likely: the books as a real list
  model, updated in place, which multi-selection (goal 3) needs anyway.

Done when a 50 000-book library loads in a few seconds and typing in the
search, sorting and saving never block the interface noticeably.

### 2. Search like Calibre

Calibre users know its search language and keep saved searches and virtual
libraries in the library itself. Cousoir's search only finds words.

- Calibre's search language, ported from `search_query_parser.py` and
  `db/search.py`: `field:value`, `=exact`, `~regex`, `and`, `or`, `not`,
  parentheses, comparisons of ratings, sizes, numbers and dates, `true` and
  `false`, `formats:`, `identifiers:`, `languages:`, `search:` and `vl:`.
  Plain words keep finding what they find today.
- The saved searches and virtual libraries stored in `metadata.db`, as
  Calibre saves them: the saved searches in the sidebar, a virtual library
  narrowing every section, as in Calibre.
- `cousoir-cli search` prints the matching ids, as `calibredb search` does;
  the golden test runs a battery of queries through both.

Done when every query of the battery finds the books Calibre finds.

### 3. Many books at once

One book at a time is the main limit of daily use.

- Selecting several books: Ctrl and Shift clicks, Shift with the arrows,
  Ctrl+A, in the table and the covers; the status bar counts them.
- The actions on all of them: move to the trash, convert, send to a reader.
- Editing them together, as Calibre's bulk metadata edit does: authors,
  series with numbering, tags added and removed, rating, publisher,
  languages, publication date.
- The golden test makes the same bulk edits with Calibre's writers and with
  `cousoir-cli apply`.

### 4. Editing the description

The description is the one field the details pane cannot edit.

- An editor for it on the book's page, with paragraphs, bold, italics,
  links, lists and headings, saved as clean HTML; plain text pasted becomes
  paragraphs as Calibre's `comments_to_html()` makes them.

### 5. Custom columns

Many Calibre libraries have custom columns, and Cousoir neither shows them
nor adds or restores books in such a library.

- Reading every kind of custom column, and `cousoir-cli list` matching
  `calibredb list` on a fixture library that has one of each kind.
- Showing them in the details pane and as table columns; searching them
  with `#name:`.
- Writing them through a port of Calibre's custom column writers, which
  lifts the refusals to add and restore books in such libraries.

### 6. Reading life from the readers

What a Kobo knows about each book, read only, as the Kobo database stays
untouched: whether it was read, how far, when; its highlights and notes.

- The book's page says where the reader is in the book; the reader's page
  shows it per book.
- Highlights and notes shown per book and exported as Markdown.
- A Kindle's `My Clippings.txt`, read over MTP, for the same.

### 7. A command palette

Ctrl+K: books, authors, series, tags, sections and actions, found by a few
letters. Keyboard-first, as the rest of Cousoir.

### 8. Calibre's templates

Composite columns are built by Calibre's template language, and `template:`
searches run it. Cousoir shows no composite and cannot search one.

- A port of the template language's single function mode and general
  program mode, with the functions most templates use, checked against
  Calibre's own formatter on a battery of templates.
- Composite columns shown, searched and written into `metadata.opf` as
  Calibre computes them; `template:` searches.

### 9. Metadata and covers from the web, through Calibre

Calibre fills a book's metadata and cover from Google, Amazon and other
sources; Cousoir cannot. Calibre does the fetching, run by `calibre-debug`
as for the conversions: the jobs of its own Edit metadata dialog.

- A Download action on a book, run in the background: the sources' answer
  shown beside the book's own values, each field picked or left, the cover
  among the ones found; saved as an edit is, with Undo.
- No network in the tests: a fake `calibre-debug` answers with books and
  covers.

### 10. The list's columns

The table shows a fixed set of columns.

- A chooser of the columns shown and their order, custom columns
  included, kept per library.
- Sorting by any of them as Calibre sorts it, checked against Calibre's
  `multisort()`.

### 11. Saves stay instant at any size

A change re-reads the whole library: 1.1 s at 50 000 books, on the worker
thread, before the window shows it.

- Re-read only the books a change touched, and the items they link to.
- The window shows the saved value at once; measured by the benchmark.

### 12. Calibre's language names

Languages show and search by English names. Calibre has them in its own
language, from its ISO 639 translations.

- Language names and their search in Calibre's language, from tables
  generated from Calibre; `language_strings()` in templates too.

### 13. Notes

Calibre 7 keeps notes on authors, series, tags and publishers in
`.calnotes`. Cousoir shows none.

- An item's note shown beside its books, read from `notes.db` as Calibre
  reads it; a search on notes later.

## Second plan: useful without Calibre

After goal 12 the user asked for ambition over polish: features people
feel, Calibre's library format and interoperability kept exact, but
Calibre itself needed for less. Goals 11 and 13 stay; these come first.

### 14. A Kobo without Calibre

Sending to a Kobo needs Calibre today: its job converts to KEPUB, puts the
library's metadata in the copy, picks the file's path and writes
metadata.calibre. Cousoir does it all itself; Calibre, when installed, is
no longer needed for it.

- KEPUB conversion in C++, after Calibre's `kepubify.py`: sentence spans,
  the book-columns wrappers, Kobo's styles, a title page when there is none.
- The library's metadata and cover put into the copy, as Calibre's EPUB
  metadata writer does.
- The KoboTouch driver's file path and metadata.calibre entry.
- Checked by the golden test against Calibre's own send: the Kobo each
  leaves must be read alike by both.

### 15. The library on the local network

An OPDS catalog served by Cousoir, for KOReader, phone readers and other
e-readers: books by author, series, tag and search, covers, downloads; a
simple web page to browse and download from a browser. No Calibre.

### 16. Metadata and covers from the web without Calibre

Cousoir's own sources, Open Library and Google Books over their public
APIs, in the dialog of goal 9 when Calibre is not installed or beside
Calibre's results. Tests against a local fake server.

### 17. Highlights from a Kindle

The Kindle's "My Clippings.txt" read like the Kobo's notes: by book,
copied or saved as Markdown.

### Always

- Bugs found on the way are fixed, with a test.
- DOC.md and the README describe what was added.

## Status

| Goal | State |
|---|---|
| 0. Sandbox and baseline | done |
| 1. A large library stays instant | done (see the journal); memory left for later |
| 2. Search like Calibre | done; follow-ups below |
| 3. Many books at once | done |
| 4. Editing the description | done |
| 5. Custom columns | done; as table columns, later |
| 6. Reading life from the readers | done for the Kobo; a Kindle's clippings, later |
| 7. A command palette | done |
| 8. Calibre's templates | done |
| 9. Metadata from the web | done for one book; several at once, later |
| 10. The list's columns | done |
| 11. Instant saves | planned |
| 12. Language names | done |
| 14. A Kobo without Calibre | done; a PDF goes without metadata |
| 15. The library on the network | done |
| 16. Metadata from the web without Calibre | planned |
| 17. Kindle highlights | planned |
| 13. Notes | planned |

## Journal

### 2026-10-22

- Created `experimental` from `master` (7664f54).
- Baseline: `make build` in 47 s, 12 jobs; the 15 tests pass in 55 s.
- The tests wrote outside the source tree: `~/.qttest` (Qt's test mode),
  `/tmp` (temporary folders) and `~/.cache/cousoir-golden` (the golden
  test's copies). Added `tools/sandbox.sh`, which moves the home, temporary
  and XDG folders and Calibre's configuration under `build/sandbox`. The 15
  tests pass under it in 57 s, and nothing outside the tree changed.
- Calibre is installed as its binary build, whose Python sources are
  frozen: the sources ported from are fetched from Calibre's repository,
  at the tag of the version installed, into `build/sandbox/calibre-src`.

### 2026-10-22 — goal 1, a large library (`exp-scale`)

`cousoir-test-tool make-library` writes libraries of made-up books through
Calibre's schema and triggers (50 000 books in 7 s, 350 MB, covers
hard-linked). Calibre lists a generated library exactly as Cousoir does: the
golden test now compares 2000 generated books too. `cousoir-benchmark`
measures the model and, with `--ui`, the window until each frame is drawn.

At 50 000 books, before and after (ms; the window's figures include
drawing, offscreen with the software renderer):

| Step | Before | After |
|---|---|---|
| load, worker thread | 874 | 780 |
| check, worker thread | 8851 | 793 |
| search, slowest letter, model | 144 | 7 |
| search, slowest letter, window | 427 | 46 |
| clear the search, window | 517 | 20 |
| sort, model | 115–162 | 1–27 |
| sort, window | 496–513 | 24–60 |
| save an edit, worker thread | 1012 | 916 |

What was slow, and what changed:

- The books reached QML as a list of maps, built again for every book on
  every keystroke and sort, and the views took it whole. They are now a
  `BookListModel`, whose rows point into the library's books (shared, not
  copied) and make a row's map only when a row is shown. The same books in
  the same order only update their values, so a save keeps the list where
  it was.
- Sorting compared strings with the collator, a million times for 50 000
  books. The collation keys are made once per load, on the worker thread
  with the folded search texts, and the sorted order is kept until the sort
  or the books change: a search only filters it.
- `findBook()` walked the books; it uses the index's id table.
- The check compared author names through initials by working out both
  names' author sorts and words in the innermost loop: quadratic in the
  authors sharing a surname. It now compares parts worked out once.
- The views kept their place on a new list because a new array reset them;
  a model reset does not, so the books page keeps the current row, or the
  first, itself.

Found on the way:

- Qt logs to the system journal when its error output is not a terminal;
  `tools/sandbox.sh` sets `QT_FORCE_STDERR_LOGGING`.
- A save at 50 000 books once took 24 s: the `fsync()` of the library's
  folder, on btrfs, also commits whatever else is pending, here the copy of
  120 000 files the benchmark had just made. Not Cousoir's to fix; the
  benchmark flushes first.
- The lists do not handle Page Up, Page Down, Home and End: left for goal 3,
  with the selection keys.

Left for later: a save reads the whole library again (readLibrary 240 ms,
findFiles 210 ms, the index 100 ms at 50 000 books), on the worker thread;
and a save holds two copies of the books and of the database at once, 700
MB at 50 000 books.

### 2026-10-22 — goal 2, search like Calibre (`exp-search`)

Calibre's binary build has no Python sources to read: the files ported
from were fetched from Calibre's repository at v9.16.0.

- `calibre_search.cpp` ports the parser of `search_query_parser.py` (its
  tokens, quirks included: a quoted "or" is an operator, `title:foo "bar"`
  searches "bar") and the matches of `db/search.py` for every built-in
  field, with the candidate-set evaluation of `and`, `or` and `not`.
- Matching text is where a port could not follow code: Calibre asks ICU's
  collators. `tools/oracle/search_oracle.py` asks them instead, character
  by character, what each folds to, and writes `calibre_search.inc` (4723
  folds and the date words of 40 languages). A match may not start or end
  inside a character folded to several ("s" is not in "ß", "ss" is), as
  ICU's cannot. On 68 445 query and text pairs in many scripts the folds
  agree with Calibre's two matchers, after three rounds: queries of
  punctuation only match everything; expansions keep their boundaries;
  katakana fold to the hiragana's own fold.
- The golden test runs 328 searches through `cache.search()` and Cousoir's,
  on the fixture and on 2000 generated books with saved searches, virtual
  libraries, grouped terms and user categories: all alike. 208 of them find
  some books but not all. Calibre itself crashes on `series_sort:true`
  (`KeyError`), which the comparison leaves out.
- The window: the search box speaks Calibre's language, and says why when
  Calibre's search could not read the text; the virtual libraries are in a
  box beside it, narrowing every section and count, remembered per library,
  Calibre's start choice by default; the saved searches are a sidebar group
  with their counts. `cousoir-cli search` answers as `calibredb search`.
- Cost at 50 000 books: 23 ms a letter in the model, 39 to 56 ms with the
  frame, all fields searched, descriptions included; loading takes 240 ms
  more on the worker thread to fold the texts, and 50 MB more.
- Found on the way: a UI test clicked a header in the frame it reappeared,
  before it was laid out, and failed one run in six once the list became a
  model; and the search error did not change from one failing text to
  another, as it was notified only with the rows.
- `cousoir-benchmark --screenshot` draws the window to a file, in a
  section, with a search, a virtual library or a book open: to look at
  what was made.

Follow-ups for the search: custom columns (goal 5); Calibre's ISO 639
tables, for language names in Calibre's language and the rare languages,
which would make `canonicalizeLanguage()` exact too; folds per interface
language, as Swedish tells "a" from "å" and Spanish "n" from "ñ";
`template:`.

### 2026-10-22 — goal 3, many books at once (`exp-bulk`)

- `BulkEdit` ports the part of Calibre's bulk metadata edit
  (`MyBlockingBusy.do_all()`) that sets fields: authors, rating, publisher,
  series with its three numberings, publication date, languages, tags added
  and removed by `bulk_modify_tags()` with `cleanup_tags()`. A bulk edit
  goes to the commit as such and is worked out there, on the database as the
  changes before it in the transaction left it: worked out in the window, two
  bulk edits in a row would each start from the library before both, and the
  second would undo the first.
- The golden test makes the eight bulk edits of `tests/data/edits/bulk.json`
  with Calibre's functions and with `cousoir-cli apply`: identical tables
  and files, case changes, renamed series and moved folders included. Only
  the order of `metadata_dirtied`, the queue of books Calibre writes again,
  differed: Calibre queues them as each `set_field()` reaches them. The
  comparison now looks at the books queued.
- The selection lives in `BookListModel`, by book id: a sort keeps it, a
  search drops what it hides, a select-all of 50 000 rows is one signal. The
  table and the covers take Ctrl and Shift clicks, Shift with the moving
  keys, Ctrl+A, and now Page Up, Page Down, Home and End (missing before,
  noted in goal 1). The current book stays current through a sort.
- Del, Ctrl+D, Ctrl+Shift+K and Ctrl+Shift+A and the row menu act on the
  selection; the pane beside the list edits it, with Undo.
- Found on the way: a binding on a function call does not follow the
  selection; `selectedIds` became a property.

### 2026-10-22 — goal 4, the description (`exp-description`)

- The description edits as text in the book's page, in the monospace look
  of the rest: blank lines between paragraphs, a line break where a line
  ends, and Markdown's bold, italics, links, lists and headings. No toolbar,
  no rich-text widget.
- Qt reads Markdown and writes it, but its writer wraps lines at 80 columns,
  which would turn every long paragraph into line breaks, and its reader
  makes a paragraph of each line break. `description.cpp` uses QTextDocument
  only to read the HTML, walks its blocks and fragments to write the text,
  and reads the text back with a small parser of its own. A description
  saved from its text reads back as the same text (tested), one saved
  unchanged is not rewritten, and one holding a table or an image says so
  before it is saved.
- Revert changes now puts the description back too.

### 2026-10-22 — a review of goals 1 to 3 (`exp-review`)

An agent reviewed `master...experimental` with fresh eyes. What it found,
each checked against the code and fixed:

- The current row was always drawn selected, but actions used the selection
  only when the current row was in it: Ctrl+click the current book out of
  three selected, and Del or Ctrl+D acted on that book alone, the one
  unselected. Now the selection is what actions act on, always, and a
  current row outside it is outlined; after a search, the current row is the
  first book still selected. Tested.
- Undo of a bulk edit put back every field it could change, from values
  read before the edit was even saved: a rating changed since was undone
  too, and two bulk edits in a row were undone as one. The commit now keeps
  the books as they were inside its transaction; the library keeps the
  fields the edit set, before and after; undo puts back a field only while
  it still holds what the edit left. Tested.
- A virtual library Cousoir cannot search (a custom column's, until goal 5)
  showed no book at all, silently; it now leaves the books whole and says
  why, and a saved search that cannot run says so instead of "No books
  here".
- When the saved search shown disappeared, the library went back to its
  books but the window's section did not follow.
- The row menu acted on a row number: a list changing while it was open
  moved the action to a neighbour. It keeps the books as it opens.
- The bulk pane kept what was typed for the books before.
- The load and save watchers kept the previous result, a whole library and
  its index, alive; `rebuildRows` compared every id twice; the counts with a
  virtual library walked every book on each read.

And one more, found while chasing a test that failed once: a detail field
bound its text to the stored value, which typing does not break, so a field
that follows another, the title sort its title, lost what was being typed in
it when the other's save ended. The field takes the stored value only while
nothing is typed. A test fails without the fix.

### 2026-10-22 — goal 5, custom columns (`exp-custom`)

- Every kind of custom column is read as Calibre's tables read it, and
  `cousoir-cli list` prints them as `calibredb list` does: byte for byte on
  `tests/data/library-custom`, a copy of the fixture with a column of each
  of Calibre's thirteen kinds, made and filled by `calibredb`
  (`tools/make_custom_library.sh`). The search takes `#name:` as Calibre
  does; the battery gained 77 searches on custom columns, all alike.
- Writing goes through a port of Calibre's custom column writers: the
  adapters, the item tables of text, series and enumerations, a series'
  number kept in its link, an enumeration that takes only its values. The
  `metadata.opf` files carry the columns as Calibre's
  `serialize_user_metadata()` writes them, byte for byte against Calibre's.
  `tests/data/edits/custom.json` edits each kind with both programs: same
  tables, same files.
- The details pane shows and edits them, each by its kind.
- Adding a book reads the values its file holds for custom columns, from an
  EPUB's OPF of either version or a PDF's XMP packet, merges them as
  `smart_update()` does, and gives the book the values the library's columns
  give new books. Restoring a book from the trash takes them back from its
  `metadata.opf`. Both were refused before. The golden test reads files
  Calibre exported from a library with custom columns, some of them not
  matching the fixture's (another kind, other separators, a value an
  enumeration does not have, a column it does not have), adds them with
  default values set, trashes and restores books, and crosses the trashes:
  the same tables and files, but for the composite column's value.

Found on the way:

- `calibredb add` loses everything in a PDF's XMP packet under Calibre's
  own names, custom columns, rating, series, title sort: importing
  `calibre.db.cache` breaks the namespace prefix of the XMP reader's XPath,
  in that process only. Calibre's Add books reads the files in a worker
  that never imports it, and keeps them. Cousoir does as Add books does;
  the oracle reads the files to add before it opens the library.
- The `metadata.opf` of a book in a library with a composite column of
  several values gave it the separators of text.
- A pointer into a temporary JSON value, freed at once, corrupted the heap
  when a column had a default value. `find()` on a temporary no longer
  compiles.

Left for later: custom columns as table columns, to see and sort them in
the list; composite columns, whose values Calibre computes from templates
(goal 8). The golden comparison leaves composite values out of the
`metadata.opf` files.

### 2026-10-22 — a review of goal 5 (`exp-review2`)

An agent reviewed the custom columns with fresh eyes. What it found, each
checked against the code and Calibre's:

- Revert failed whole when a custom date had been cleared: the undefined
  date was kept as a text the date writer refused. It is kept as nothing,
  which Calibre's adapter makes undefined again, and the writer takes the
  undefined date's text too. Tested.
- A custom date with a time and no zone was taken as UTC; Calibre's
  `parse_date(assume_utc=False)` takes it as local. And the pane accepted
  dates (`2024/03/06`) the writer then refused: both use one parser now.
  Tested.
- Every save rebuilt every custom column's field, losing focus and what was
  typed in the next one. The fields now follow the columns' keys, which
  change only with the library's columns. Tested.
- A long text column holding a table or an image lost them when edited from
  its text, silently. It is read-only in the pane, which says why, as the
  description warns before saving. Tested.
- Floats were laid out as Qt's shortest form, `1e+05` where Python writes
  `100000.0`, in `cousoir-cli list` and the OPF files. `floatRepr()` now
  follows `repr()`; the custom fixture has 100000 and 0.00001, which the
  golden test lists as `calibredb` does.
- A whole-star rating column showed "None" for an odd rating set elsewhere;
  the stored value is always among the choices. Tested.
- Not changed: custom dates lose their fraction of a second when read, but
  so do Calibre's (`c_parse()` in `db/tables.py` reads to the second), so
  lists, OPF files and restored books match Calibre's. Words alone not
  looking in composite columns is documented, until goal 8 computes them.

### 2026-10-22 — goal 6, reading life from the readers (`exp-reading`)

- A Kobo's books come with what its database says of their reading: status,
  percent, last read, time spent, and the highlights, notes and bookmarks,
  read as Calibre's Kobo `Bookmark` reads them to fetch annotations, quirks
  included: a hidden note left out, a corner turned down as a bookmark, an
  empty highlight kept, chapters of a KEPUB from its rows of type 899, a
  note taking another's place when two chapters share an index.
- The golden test reads `tests/data/kobo-reading.sql` on a fake Kobo with
  Calibre's `Bookmark` and with `cousoir-cli device`: 4 books, 12 notes
  alike. `kobo.sql` now has the Bookmark table of firmware 4.38, which
  Calibre's query needs.
- A book's page shows each reader's copy: how far, when, how long, and the
  notes by chapter, copied or saved as Markdown. The Kobo's page has Read
  and Notes columns. What a Kobo told is kept per library outside it, so it
  stays once the Kobo is unplugged; nothing is written to the library or the
  Kobo.
- Not done: a Kindle's `My Clippings.txt`. Calibre has no reader for it to
  test against, its format changes with the Kindle's language, and a
  Kindle over MTP is opened for each thing Cousoir does there. Left for
  later, with writing the notes into Calibre's annotations, which would
  need the Kobo's positions turned into the CFIs Calibre's viewer uses.
- Found on the way: run by hand without ctest's environment, the UI tests
  draw on the real display, and one fails at 200%; they are run with it.

### 2026-10-22 — goal 7, a command palette (`exp-palette`)

- Ctrl+K opens a palette: the library's books, authors, series, tags and
  publishers, and the window's sections, virtual libraries and the actions
  that can be done now, with their shortcuts. Letters are folded as the
  search folds them, so `ecume` finds "L'Écume des jours", and a book is
  found by words of its title and of its authors together (`ecume vian`).
  A name that is what was typed, or starts with it, comes first; commands
  before items, items before books.
- `Palette::search()` reads the folded titles and names the search index
  already has: 9.5 ms a letter at 50 000 books, which the benchmark now
  measures.
- `cousoir-benchmark --screenshot` takes `--key` and `--type`, to draw the
  palette open.

### 2026-10-22 — a review of goals 6 and 7 (`exp-review3`)

An agent reviewed the Kobo's reading and the palette. What it found, each
checked and fixed:

- A palette pick from an open book, or for the section shown, did nothing
  visible: the book stayed open over the list it changed. The book now
  closes first, and the section shown goes back to its list. Tested.
- Leaving a book through the palette dropped a description being edited.
  It is saved first. Tested.
- The reading log could lose everything a Kobo told: recorded while the
  library was still loading, or had failed to, no book matched and the
  Kobo's entries were replaced by none. It is recorded only once the
  library is in.
- The log was written again on every save, on the window's thread, looking
  up each book through all the library's: it is now written when a Kobo
  tells something new or first on a day, with the books found by id.
- The book page's reading followed the reader's whole list, sorted again on
  each change; it follows a count of readers now.
- Calibre rounds a chapter's progress with Python's round(), a tie to the
  even digit: 3.125 is 3.12, which Cousoir made 3.13. The fixture has such
  a tie now, read alike by both.
- Save proposed a broken file name for a title with "#"; a reader that
  failed to load hid what it had told before.

### 2026-10-22 — goal 8, Calibre's templates (`exp-templates`)

- Calibre's template language is ported from `utils/formatter.py` and
  `formatter_functions.py`: the single function mode on Python's
  `string.Formatter`, with Python's format specs (`{series_index:05.1f}`),
  prefixes and suffixes and old style function calls; the program mode's
  lexer, parser and interpreter, node by node; the builtin functions; and
  the book as `ProxyMetadata` shows it to templates, its fields formatted
  as `format_field_extended()` formats them.
- What templates read besides the books is taken from Calibre by
  `tools/oracle/template_tables_oracle.py`: the built-in fields with their
  search terms and date formats, and in each of Calibre's languages the
  names of days and months (its locale data) and the words templates print.
- `tools/oracle/template_oracle.py` runs 244 templates through Calibre's
  `SafeFormat` on each book, as a composite column is computed, and
  `cousoir-cli template` through Cousoir's: all alike on the fixture, the
  custom library and 2000 generated books, an error counting as alike
  whatever its message. The first run had 241 of 244: `booksize()` of a
  book without files, `%p` in the system's language (none in French), and
  dates Calibre's dateutil reads in words (`19 oct. 2026`), now read as
  dateutil reads the forms dates take.
- Composite columns are computed when the library loads, and for the books
  a change writes the `metadata.opf` of: lists, OPF files, the details pane
  and searches have them, as text, numbers or dates by their sort, and
  split at commas when they hold several. The custom fixture has one of
  each kind; the comparisons no longer leave them out.
- `template:"TEMPLATE#@#:t:QUERY"` searches, with text, number, date and
  yes or no comparisons; the battery has 440 searches, all alike.
- Cost: a composite takes about 5 µs a book; loading 50 000 books with one
  takes 0.33 s more, and a change writes only its books' composites.

Left for later: the library's stored template functions, Python templates,
and the functions that need Calibre's window (book counts, notes, extra
files, virtual libraries).

### 2026-10-22 — a review of goal 8 (`exp-review4`)

An agent reviewed the template port and found fifteen faults the battery
let through; each was checked against Calibre and fixed, and the battery
has 107 more templates (351), all alike on the three libraries:

- A function of any number of arguments given too few read past the end of
  its list, a crash at load for a composite using it: each now names the
  least it takes, and fewer is Calibre's IndexError.
- `{:'…'}`, the field left out, failed: Python numbers it, and SafeFormat
  gives a number the empty text (Calibre's own "Formats" column preset).
- A template calling itself, or thousands of nested parentheses,
  overflowed the stack; there is a depth limit now.
- Eight functions were missing: `format_duration()`, `formats_modtimes()`,
  `formats_path_segments()`, `make_url()`, `make_url_extended()`,
  `query_string()`, `urls_from_identifiers()`, `width_from_pages()`. The
  identifier links are in `identifier_links.cpp`, for the details pane
  later.
- A composite whose display says not to keep its value in the OPF had it
  written there. The custom fixture's `#pagesort` says so now, and its OPF
  files are Calibre's again.
- `date_arithmetic('')` failed; `format_date(…, 'from_number…')` did not
  read a number; `$` was unknown in a composite's program; `re_group()`
  went one group too far with nested groups (Python's `lastindex`);
  `strcmpcase()` compared code units, not Calibre's case-sensitive
  collation; `eval('program: $a')` read the book, not the variables, and
  `with` left `lookup()` on the first book; functions in single function
  mode were stripped even in `re_group()`'s templates; `ceiling(1e20)` and
  ranges near 2⁶³ overflowed.
- A search split a composite of names at its commas; it is split at `|`,
  and the searches have such a column now.
- Lengths are counted in characters as Python counts them (`strlen('😀')`
  is 1); an empty separator is an error; `{x::'…'}` runs its program;
  `isoformat` dates keep their microseconds; `author_sorts()` looks authors
  up by id.
- The template oracle let the library go before running the templates, so
  Calibre's `with` failed; it keeps it open.

Left as it is: `program: $` alone gives the empty text where Calibre fails
on None, and an error's own text is still English.

### 2026-10-22 — goal 9, metadata and covers from the web (`exp-metadata`)

- Cousoir runs the two jobs of Calibre's Edit metadata dialog,
  `single_identify()` and `single_covers()` of `sources/worker.py`, with
  calibre-debug in a folder of the library's cache: the sources Calibre is
  set to use, updated as Calibre updates them, their caches handed from
  the first job to the second as the dialog hands them. The books found are
  read back from their OPF as the dialog reads them, so a book without a
  language gets the interface language, as in Calibre.
- `MetadataDownload::changes()` ports `update_from_mi()`. The golden test
  makes it a comparison with Calibre itself: Cousoir's identify job runs
  under Calibre with `single_identify()` answering made-up books, then
  Calibre's real dialog, shown offscreen, applies five downloads to one
  copy of the fixture and `cousoir-cli apply` to another; both libraries
  end alike. The first run found two differences: the day found is stored
  at local midnight, and the rating editor has whole stars only, 4.75
  stars making 8, not 9.
- The dialog: the books found, the one picked beside the book field by
  field with the differing fields ticked, then the covers as they come,
  the book's own first; Save is one change that Revert undoes. A globe on
  the book's toolbar and "Download metadata and covers" in Ctrl+K open it.
- `cousoir-cli download BOOK_ID [--covers]` asks the sources and prints
  what they found. Run inside `unshare -rn`, a namespace without network,
  it ran the real jobs against Calibre 9.16: every source failed at once
  and the job ended cleanly, nothing going out.
- Tests: a unit test and a UI test with a fake calibre-debug, which
  answers with what Cousoir's job wrote under Calibre.

Left for later: downloading for several books at once, as Calibre's bulk
download does, and Calibre's per-source settings.

### 2026-10-22 — a review of goal 9 (`exp-review5`)

An agent reviewed the metadata download. What it found, each checked and
fixed:

- Covers asked for a second time ran in the folder of the first: their
  covers mixed, and Calibre's `single_covers()` fails on a cover it saved
  already, which the fake did not do. Covers asked for while others came
  were refused, and the dialog showed the old ones as the new book's. A
  covers job now replaces the one before, in a folder of its own; the fake
  fails as Calibre does. Tested both ways.
- A cover picked, then Back to the fields and Save, saved the fields alone.
  The cover goes with them, and the footer says so. Tested.
- A covers job that failed said nothing; it tells why now, the covers saved
  before kept. A start failing at once could leave the spinner turning.
- A book without a cover was given the first one found unless another was
  picked; it keeps having none, as in Calibre's dialog.
- The keyboard: Space ticks a field, the arrows pick a cover, and Enter on
  a button presses it rather than saving, in the confirmation dialog too.
  Covers coming in no longer reload the grid.
- Smaller: entities in a plain summary, a description of spaces taken as
  one to add to, empty author sorts, the log of a search that found
  nothing, errors of the job's Python past its first part.

### 2026-10-22 — goal 10, the list's columns (`exp-columns`)

- `BookSort` ports Calibre's `Cache.multisort()` and the sort keys of each
  field (`db/fields.py`). `tools/oracle/sort_oracle.py` sorts the books
  with Calibre for 90 sort histories, every field and custom column both
  ways and multi-level ones, and `cousoir-test-tool sort-books` with
  Cousoir: all alike on the fixture, the custom library and the generated
  one. The comparison found what reading the code had not: the formats
  table names no items, so books sort by how many formats they have;
  Calibre reads dates to the second, so books added the same second tie;
  the built-in description is collated as text while a custom column of
  comments compares as raw text.
- Two orders change to Calibre's: books in no series come first in an
  ascending series sort, and a reversed sort keeps ties in place.
- The list keeps a sort history as Calibre's view does (five levels, the
  `maximum_resort_levels` tweak), so ties fall to the sorts before. Keys are
  made for a field the first time it sorts, on the load's thread for the
  history's fields: 38 ms the first sort by authors at 50 000 books, 5 to
  11 ms after.
- Any of Calibre's columns can show, custom ones included, from a header's
  right click: show, hide, move, sort. The columns and the sort are kept per
  library; the first time, Calibre's saved list state (`library_view books
  view state`) gives them.
- The comparison runs in English until goal 12: Calibre sorts languages by
  their names in its interface language, which Cousoir does not have yet.

Left for later: column widths of one's own, and the cover grid's sort.

### 2026-10-22 — goal 12, Calibre's language names (`exp-languages`)

- Language names come from the Calibre installed, as Calibre gives them:
  its ISO 639 table (`iso639.calibre_msgpack`) through its translator,
  `messages.mo` then `iso639.mo` of `locales.zip`. The browse section, the
  details, the list, the sort and `language_strings()` use them; a search
  takes a name in Calibre's language too. Without Calibre, Qt's English
  names. Sorts and templates now compare alike with Calibre in French.
- The tests read a fixture of those resources (`make oracle` writes it
  from Calibre), so they do not depend on Calibre being installed.
- From goal 10's review: a reload whose custom columns changed showed
  their values under the wrong headers; the list's columns follow each
  load now. The rest of that review was corner cases, left as they are.

### 2026-10-22 — goal 14, a Kobo without Calibre (`exp-kobo`)

- `Kepub` ports Calibre's kepubify on a small XML tree (Qt's stream reader
  and writer, HTML entities made references): the pages of a test book
  with lists, tables, notes, code, SVG, entities and two languages come out
  byte for byte as Calibre's. It also converts books whose container names
  a missing package first, which Calibre refuses. Without Calibre, Convert
  to KEPUB uses it, on a worker thread, the library's metadata and cover
  put into the copy.
- Sending to a Kobo without Calibre: the driver's save template through
  Cousoir's template engine, Calibre's filename cleaning and shortening,
  the book's entry in metadata.calibre as Calibre's JsonCodec writes it
  (thumbnail included), driveinfo.calibre, and the entries of books the
  Kobo took in rewritten as KoboTouch's books() rewrites them. The paths
  are Calibre's to the character.
- The golden test runs the Kobo steps a second time with Cousoir sending
  alone (`COUSOIR_NATIVE_SEND`), compared with Calibre's own sends in a
  mode that lets the KEPUBs and thumbnails be Cousoir's: alike, and
  Calibre, connecting, knows the books Cousoir sent.
- Left for later: a PDF's metadata written into the copy, and a Kindle
  without Calibre.

### 2026-10-22 — goal 15, the library on the local network (`exp-server`)

- `LibraryServer`, a small HTTP/1.1 server of Cousoir's own on
  `QTcpServer` (Qt's HTTP server module is not installed here, and nothing
  is installed outside the folder): GET and HEAD, a connection per
  request, files sent as the client takes them, thumbnails made on the
  thread pool and kept, Basic authentication with a password, read only.
- `Opds` writes the OPDS 1.2 catalog: newest, by title, authors, series,
  tags, publishers and languages (by name in Calibre's interface
  language), each leading to its books; searches in Calibre's language
  through the library's own index and sorter; pages of 50 with next and
  previous links; each book described as Calibre's content server
  describes it, and downloads named as it names them. A web page for
  browsers searches and downloads.
- Settings, Network: Share the library, the addresses with a QR code for a
  phone's camera and Copy, the port (8090, beside Calibre's 8080) and a
  password, remembered. `cousoir-cli serve` does it without the
  application and reads the library again when Calibre changes it.
- `QrCode`: byte mode, level M, versions 1 to 40; the modules of five
  addresses match libqrencode's with the same mask, and zbarimg reads
  Cousoir's own back.
- Tests: `library_server_tests` follows the catalog's links as a reader
  would on 127.0.0.1; `qr_code_tests`; a UI test turns sharing on and reads
  the catalog. The smoke test of `cousoir-cli serve` ran in a network
  namespace of its own (`unshare -rn`), loopback only.
- Docs: the stale "Converting/Sending needs Calibre" sentences of DOC.md
  now say what needs Calibre since goal 14.

### 2026-10-22 — a review of goal 15 (`exp-review6`)

A review found no crash, hang or security hole; it found four small bugs,
fixed with tests: the web page's thumbnails cached an hour past a cover's
change (now `?v=` the cover's time), languages listed by code rather than
by name, an empty library name from `cousoir-cli serve --library PATH/`,
and a log saying 200 for a file gone from the disk. It also noted that a
remembered share starts again unnoticed, on a café's network for
instance: the status bar now shows the library shared, a click away from
the Network section.

