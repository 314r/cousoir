# The experimental branch

Autonomous work on Cousoir, started on 2026-10-22, for review before any of
it goes to `master`. This file holds the goals, how the work is done, where
each goal stands, and a journal of what was done and why.

Nothing here is merged into `master`, pushed or published: the review decides.

## How the work is done

- The first `experimental` branched from `master` at 7664f54 and was merged
  into it on 2026-10-23. The second, from 2026-10-25, branches from
  `master` at 9e9dc9f and works on the reader (see "Since 2026-10-25: the
  reader" below). Each goal is worked on
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

Done with Open Library alone: Google's Atom feed, which Calibre's Google
source uses, answers 500 today, and its JSON API without a key is out of
quota for anonymous callers.

### 17. Highlights from a Kindle

The Kindle's "My Clippings.txt" read like the Kobo's notes: by book,
copied or saved as Markdown.

## Third plan: the rest of the way without Calibre

With plan 2, a Kobo, metadata from the web and the library on the network
no longer need Calibre. These go further; goals 11 and 13 stay.

### 18. A Kindle without Calibre

Sending to a Kindle by itself, as goal 14 does for a Kobo: the library's
metadata written into the AZW3 or MOBI copy as Calibre's MOBI metadata
writer does (`ebooks/metadata/mobi.py`), the cover thumbnail Amazon's
covers bug needs, the APNX page numbers by Calibre's fast method, the
file's path and metadata.calibre. The golden test compares with Calibre's
own send.

### 19. Reading in Cousoir

A reader for EPUB (Qt WebEngine) and PDF (Qt Pdf), opened from a book's
page: pages, the table of contents, a search, the position kept. The
position and the highlights are kept where Calibre's viewer keeps them,
in the library's `last_read_positions` and `annotations` tables, so that
each opens where the other stopped and shows the other's highlights.
Built only when Qt WebEngine is there.

### Always

- Bugs found on the way are fixed, with a test.
- DOC.md and the README describe what was added.

## Fourth plan: more readers, more of the library

Plans 2 and 3 took the readers plugged in, the web and reading away from
Calibre. These reach the readers that are not plugged in, and make more of
what the library holds.

### 20. Reader apps over the network

KOReader, Calibre Companion and the other apps speak Calibre's wireless
device protocol (`devices/smart_device_app/driver.py`): Cousoir as its
server, the apps' books listed with the driver's metadata cache, books sent
with their metadata and covers, removed, metadata changed in the library
sent on connecting. Then the files themselves as Calibre sends them: the
library's metadata and cover written into the EPUB, by a port of Calibre's
OPF 3 metadata writer (`ebooks/metadata/opf3.py`), which a Kobo's plain
EPUBs need too. The golden test has the same stand-in for KOReader connect
to Calibre's server and to Cousoir's.

### 21. The library's highlights

Every highlight and note the library keeps, from Calibre's viewer, from
Cousoir's reader and from the readers: browsed, searched with Calibre's
full text index of annotations, and exported, as Calibre's "Browse
annotations" does.

### 22. Searching the books' text

Calibre's full text search (`full-text-search.db`): its index read and
searched from Cousoir, and books indexed as Calibre indexes them, so that
Calibre keeps the index Cousoir adds to.

## Since 2026-10-25: the reader

The user asked for autonomous work on the EPUB reader of goal 19: plan two
goals, work on the one that matters more, then plan two again from where
the reader stands, until the goals planned look futile, useless or harmful.
Each round's two goals and the one picked are written here; the one left
may come back in a later round.

Where the reader stood on 2026-10-25, tried on three books of Project
Gutenberg (in `build/sandbox/books`, never committed): one column the
window's whole width, about 110 characters a line at 900 pixels and twice
that full screen; text justified without hyphens; the size by WebEngine's
zoom, forgotten when the book closes; the theme's colours only; nothing
told of the page but a percentage; no search, no notes on highlights, no
bookmarks, no footnotes but by jumping to them, no reading aloud.

### Round 1

#### R1. A page made for reading — picked, done

What makes every minute of reading better, and what every reader app has:

- Text laid out at a measure that reads well: a line of about 65 to 75
  characters at most, the page centred in a wide window, and two pages side
  by side when the window holds two.
- Reading settings, kept for every book and applied at once without losing
  the place: the font (the book's own, serif, sans serif or one installed),
  the size, the line spacing, the width of the line, the alignment (the
  book's, left, justified) with hyphenation by the book's language, the
  page's colours (Cousoir's theme, light, sepia, dark, black), one page or
  two, pages or a scroll.
- What the page is: the chapter, the page in it, the time left in the
  chapter and in the book from how fast one reads, in a footer.
- Full screen (F11), the toolbar showing when the mouse goes to the top.
- Hyphenation was planned, and dropped: Qt WebEngine never gives Chromium
  its hyphenation dictionaries (`ContentBrowserClientQt` does not implement
  `GetHyphenationDictionary()`), so `hyphens: auto` does nothing; soft
  hyphens put in by the reader would shift every CFI.

Done when real books read well at any window size in every theme, the
place holds across every change of setting and size, and the tests cover
the settings, the spreads and the scroll.

#### R2. Find in the book

- A search through all of the book's text, accents and case folded as
  Calibre folds them, the results listed with their context by chapter, the
  one picked shown and marked on its page, F3 and Shift+F3 through them.
- Back and Forward after following a link or a result (Alt+Left, Alt+Right).

Left for a later round.

### Round 2

With the page laid out well, what the reader lacks most is a way through
the book other than turning pages, and a way to keep more than a yellow
mark.

#### R2. Find in the book — picked, done

From round 1. Searching a book is what one cannot do without once one
needs it; Calibre's viewer has it, and nothing in Cousoir can stand in for
it, while highlights at least exist. The results are placed by the CFIs
Cousoir's C++ already works out as the reader's script does (goal 19's
`EpubCfi`, checked against the page by `encodesCfisAsTheReader`), so a
result is found on its page exactly, without the page's script searching.

- A Search tab beside Contents and Highlights (Ctrl+F): every place of the
  book's text, case and accents ignored, white space as one space, listed
  in the book's order under their chapters with the words around them; the
  one picked shown on its page and marked there, the others of the page
  marked lightly; Enter, F3 and Shift+F3 go through them.
- Back and Forward (Alt+Left, Alt+Right, and a Back button while there is
  somewhere to go back to) after a link, a result, a table of contents
  entry, a highlight or the progress bar: a footnote read, back to the page.

Done when a search of a whole Gutenberg novel lists its results at once,
each goes to its exact place, and the tests cover the search, its marks and
the way back.

#### R3. Highlights to work with

- A bar over the selection: the five colours of Calibre's viewer, a note,
  copy; a highlight clicked: its colour changed, its note written or
  changed, removed.
- Notes kept in the viewer's JSON (`notes`), shown in the sidebar, edited
  there too, and in the library's Highlights section, as Calibre's viewer
  and "Browse annotations" show them.
- Bookmarks, as the viewer keeps them (`bookmark` annotations), listed and
  gone to.

## Status

| Goal | State |
|---|---|
| 0. Sandbox and baseline | done |
| 1. A large library stays instant | done (see the journal); memory left for later |
| 2. Search like Calibre | done; follow-ups below |
| 3. Many books at once | done |
| 4. Editing the description | done |
| 5. Custom columns | done; as table columns, later |
| 6. Reading life from the readers | done for the Kobo; a Kindle's clippings with goal 17 |
| 7. A command palette | done |
| 8. Calibre's templates | done |
| 9. Metadata from the web | done for one book; several at once, later |
| 10. The list's columns | done |
| 11. Instant saves | planned |
| 12. Language names | done |
| 14. A Kobo without Calibre | done; a PDF goes without metadata |
| 15. The library on the network | reverted on 2026-10-23 |
| 16. Metadata from the web without Calibre | done, with Open Library |
| 17. Kindle highlights | done |
| 13. Notes | done; notes with images are read, left to Calibre to change |
| 18. A Kindle without Calibre | done; a PDF goes without metadata |
| 19. Reading in Cousoir | done; notes on highlights, search and other colours later |
| 20. Reader apps over the network | reverted on 2026-10-23 |
| 21. The library's highlights | done; the readers' notes and editing a note later |
| 22. Searching the books' text | planned |

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

### 2026-10-22 — goal 16, metadata from the web without Calibre (`exp-openlibrary`)

- `OpenLibrary`: the search by ISBN (then by title and author when Open
  Library does not know it) with Calibre's own search words, checked
  against Calibre's `get_title_tokens()` and `get_author_tokens()` by a new
  oracle; the work with its best edition in the book's language; the
  work's description, Markdown made HTML; subjects as tags, `series:` ones
  as the series; MARC's language codes ("fre") made Calibre's ("fra");
  the ranking of Calibre's `InternalMetadataCompareKeyGen`; covers.
- `MetadataDownloader` asks it when Calibre is not installed: the same
  dialog, fields and saves (Calibre's `update_from_mi()` port of goal 9),
  Calibre's download preferences when it left some. `cousoir-cli download`
  too.
- Tests: `metadata_web_tests` against a fake Open Library serving answers
  the real one gave (`tools/make_openlibrary_fixtures.sh`), and an
  end-to-end download saved to a copy of the library in
  `book_library_tests`. One manual run against the real site found
  *Shadows Linger* with its publisher, date, series and description.

### 2026-10-22 — goal 17, highlights from a Kindle (`exp-clippings`)

- `KindleClippings` reads "My Clippings.txt": the book's title and
  authors ("Vian, Boris" or "Pratchett, Terry;Gaiman, Neil" made names and
  an author sort), the kind, page and location (with the older "Loc.
  1702-05"), the date in seven of the Kindle's languages, the text. By
  book: a highlight's last version over the ones it replaced, a note on
  the highlight it was made on, notes alone, bookmarks, in the book's
  order; emptied highlights left out.
- The Kindle's clippings become its books' notes, matched with the library
  as Calibre matches a reader's books (curly apostrophes and all); those of
  books gone from the Kindle are kept too, and the reading log keeps both
  once it is unplugged. A note's place shows as "page 12, location
  170-172", on the book's page and in the Markdown.
- Tests: the parser and grouping on a fixture in three languages
  (`kindle_tests`), and a folder standing for a Kindle in
  `book_library_tests`: the notes on the book's page and the Kindle's, and
  after unplugging.

### 2026-10-22 — a review of goals 16 and 17 (`exp-review7`)

Five bugs, fixed with tests: `cousoir-cli download --covers` without
Calibre waited forever for a book without covers (an answer known at once
came before the event loop ran: now queued, and dropped once stopped); a
Kindle Calibre holds kept its clippings, shown twice after a rematch; the
same book under two headings of My Clippings.txt lost one heading's notes
(now both, in order); a clipping's date, the Kindle's local time, was read
as UTC and could show the day before (now with this computer's offset);
and Open Library's dates without a day took the first, the day before in
UTC east of Greenwich: now as Calibre's `parse_only_date()`, the 15th, never
the first or last day of a month.

### 2026-10-22 — goal 18, a Kindle without Calibre (`exp-kindle`)

- `MobiMetadata` ports Calibre's `MetadataUpdater`: the EXTH records, the
  title, the first record rebuilt and the records moved, the cover and
  its thumbnail rescaled into their records. A new oracle has Calibre
  write three metadata cases into five books (its conversions, one without
  an ASIN, one without an EXTH header) with its random ASIN fixed: Cousoir's
  books are Calibre's byte for byte, the cover images aside. The same
  oracle found a Calibre bug: a MOBI without an EXTH header loses its text
  (the file's slice keeps its first length); Cousoir keeps it. Calibre's
  thumbnail names and fast APNX files match too.
- `KindleSend::prepareNatively()` follows the MTP driver: its settings
  (the Kindle's own, Amazon's defaults, the global ones), its folder and
  template through the generic `create_upload_path()` now shared with the
  Kobo (`NativeSend`), the thumbnail with its copy and index against
  Amazon's bug, the APNX, metadata.calibre (lower case paths, as MTP books
  have) and driveinfo.calibre with its `mtp_prefix`; then the job's own
  operations, which `apply()` does over the kept connection.
- The golden test sends to a Kindle natively too (`kindle-native`): every
  step alike with Calibre's send, the AZW3 compared record by record, and
  each program knows the other's books.
- Without Calibre, sending now works to both readers; conversions to AZW3
  still need Calibre.

### 2026-10-22 — a review of goal 18 (`exp-review8`)

The MOBI writer came out line for line Calibre's, and the Kobo's send
unchanged by the shared code. Two problems, fixed: a metadata.calibre (or
driveinfo.calibre) on the Kindle that could not be read was taken for
none, so the send wrote one holding only the books just sent; it now stops,
as Calibre's job does, tested with a file that cannot be read. And every
book was read whole into memory, a MOBI up to three times over: now only a
MOBI is read, to write into it, with one copy at most as its records move;
a PDF or a comic goes as a file copy.

### 2026-10-22 — goal 19, reading in Cousoir (`exp-reader`)

- `EpubBook` reads an EPUB's container, package, spine and table of
  contents (EPUB 3 nav or NCX, nested), and its files on demand.
  `BookReader` serves them to Qt WebEngine under `cousoir-book://`, and
  `ReaderWindow` shows them a screen at a time: CSS columns a page wide, the
  keys, clicks and wheel, the theme's colours, the table of contents with
  the section being read, text sizes, links. Built only with Qt WebEngine;
  PDFs through Qt Pdf's `PdfMultiPageView`.
- Positions as Calibre's viewer counts them: its `cfi.pyj` ported to
  `assets/reader/reader.js`. Calibre's renderer, run on the fixture, keeps
  each document's elements and text nodes, so the CFIs count the same
  nodes. Cousoir keeps its position, writes it to `last_read_positions` when
  the book closes (a new commit operation, as Calibre's
  `save_last_read_position_to_cursor()`), and opens where the newest
  position, its own or the viewer's, left it.
- Highlights as the viewer keeps them: rows of `annotations`, user type
  `local`, user `viewer`, the viewer's JSON, searchable text and
  `annotations_dirtied`; their ranges found before any is wrapped, as a
  wrapper would shift the text a later CFI points to; removed as the
  viewer removes one.
- Tests: `epub_book_tests`; in the UI tests, the reader on a new fixture
  book (`tools/make_reader_fixture.py`): pages, the position kept in the
  library and found again, a newer one of Calibre's viewer taken, a
  highlight made, written as the viewer writes it, shown again and
  removed; and a PDF.

### 2026-10-22 — Calibre's full text tokenizer (`exp-fts`)

Studying notes for goal 13 showed that Calibre registers its own FTS5
tokenizer under SQLite's names, `unicode61` and `porter`, on every
connection: the annotations' index in metadata.db is made with it, not
with SQLite's. The highlights Cousoir's reader writes since goal 19 were
indexed by SQLite's tokenizer, word for word different on French text
(apostrophes, accents, "ß"): Calibre, rewriting such a row later, would
have removed words the index never had, which damages an FTS5 index.

`CalibreFts` ports `sqlite_extension.cpp`'s tokenizer (ICU's word breaks,
case folding, words also without their diacritics, Snowball's stemmers)
and registers it as Calibre does on every connection Cousoir writes
through. A new oracle tokenizes ten texts, accented, CJK, Thai, Greek,
with Calibre's own, in English and French, plain and stemmed: the port
gives the same terms at the same places. Cousoir now needs ICU and
Snowball's libstemmer, both of which Calibre itself needs.

### 2026-10-22 — a review of goal 19 (`exp-review9`)

The CFI port and the highlights' JSON came out as Calibre's. Seven bugs,
fixed:

- Highlights never showed: the page's transparent backgrounds were
  important, the highlights' colours not; now important, as Calibre's are
  (seen in a screenshot of a highlight Calibre's viewer would have made).
- Opening another book while the reader was open could show the last
  one's page under its title, and save its positions as the new book's.
- A key held at a chapter's end could skip a chapter: the page being left
  still told; each message now says its page, and only the page shown is
  heard.
- Going somewhere within the chapter wrapped the highlights again, nested,
  which shifted the CFIs found in them; they are wrapped once a page loads.
- An annotation replaced kept its old words in Calibre's search index (a
  REPLACE fires no delete trigger): deleted, then inserted, as Calibre
  does.
- Each highlight was a commit of the whole library: they now wait, and go
  together a few seconds after the last, or with the position when the
  book closes, in one change.
- While the library could not be written, highlights were dropped: they
  now wait with the positions and are written the next time they can be.

And a PDF that cannot be read no longer polls itself forever; right to left
books turn their pages.

### 2026-10-22 — goal 13, notes (`exp-notes`)

Calibre 7's notes on authors, series, tags and publishers, read and
written. Cousoir reads `.calnotes/notes.db` with the library, its links to
images included; a browse section marks the items with a note, and their
books show it in a pane beside them, images and links working. Edit writes
it as the description is written, as text with Markdown's marks, and saves
it in one change, as `Notes.set_note()` would: the row, its text indexed
with the tokenizer ported in `exp-fts` (Calibre's search finds "élégantes"
by "elegantes"), the backup file with Calibre's separators, and, for a note
removed, the backup kept aside in `retired/<xxh3 of the field and name>`
with its id, which Calibre takes back when the item comes back; past 256,
the oldest go, as Calibre trims them. `CalibreNotes::hashKey()` uses xxHash's
XXH3, as Calibre's `hash_key()` does: a new dependency, small and packaged
everywhere.

Notes with images stay read-only: writing their resources means Calibre's
reference counting of `notes_resources_link` and its resource files, which
Cousoir has no use for yet, and a mistake there loses a user's image.

The fixture (`tools/oracle/notes_fixture.py`) is written by Calibre's own
API. The golden test applies three note changes with `cousoir-cli apply`
and has Calibre read, search, change and bring back the notes
(`tools/oracle/notes_check.py`), checking the index's integrity after each
step.

### 2026-10-22 — a review of goal 13 (`exp-review10`)

What Cousoir writes in `notes.db`, its backups and retired notes came out
as Calibre's; Calibre restores and unretires them. Four bugs, fixed:

- Editing a note Calibre's editor had formatted (underline, colour,
  alignment, superscript, a quotation, a rule) lost that formatting on
  save, even unchanged: such a note is now read-only, as a description
  with tables is, and a note saved unchanged is not written.
- A language's note was kept under its name in the interface language
  ("English") where Calibre keeps it under its code (`eng`): Calibre could
  not restore it, nor bring it back once retired.
- A note's images showed at their own size, cut by the pane: they now
  shrink to its width.
- A save that failed (the notes database busy or read-only) threw away
  what was typed; the editor now opens again with it.

And the notes kept aside move only once the database committed the change,
and a save that fails after writing the notes still shows them.

### 2026-10-22 — goal 20, reader apps over the network (`exp-wireless`), part 1

Calibre's wireless device protocol, the server's side. A stand-in for
KOReader (`tests/wireless/fake_koreader.py`, after KOReader's plugin)
recorded what Calibre's own server, driven as its GUI drives it
(`send_books.py` with `wireless:PORT`), tells an app over three
connections: the handshake, the driveinfo, the library's field metadata,
the books listed through the metadata cache, books sent with their
metadata and thumbnails, metadata sent on connect when the library changed
a book, books removed, the room left asked after each change, the eject.
Cousoir's `WirelessServer` and `WirelessConnection` say the same, message
for message, and keep the apps' metadata as the driver's cache does; the
golden test compares the two transcripts and the books the app kept, and
two details came from it: Calibre draws a cover for a book without one,
and sends a changed book's metadata without its type.

What Calibre tells every app alike, its extensions, the files' types and
the standard fields' descriptions, comes from Calibre by an oracle
(`assets/wireless/calibre.json`); the fields' names are translated from
Calibre's catalog, as its `_()` translates them.

In the application, Settings > Network lets apps connect; an app shows
among the readers, with its books, and takes books, removals and Disconnect
as a Kobo does. `cousoir-cli wireless` does the same from the command line,
with a file of operations as the oracle takes them.

Left for part 2: Calibre writes the library's metadata and cover into the
EPUB it sends; Cousoir sends the library's file, and the golden test
compares the files by name until it does the same.


### 2026-10-22 — a review of goal 20, part 1 (`exp-review11`)

The framing, the replies KOReader gives, the cache's keys and the datagram
answer came out right. Five bugs, fixed:

- A send waiting behind another request when the app vanished (a Kobo
  asleep, its Wi-Fi gone) was never told: the send stayed "under way" and
  every later one, to any reader, waited behind it. What waits is now told
  when the connection goes, and Stop closes the connection of a send under
  way.
- Quitting during a send to an app could crash: the server is now let go
  first, while the device library is whole.
- The uuid an app gives named the file of its metadata cache: an app on the
  network could have named any `.json` file of the user's. A digest of it
  names the file now.
- Apps were told an empty library uuid, read before the library was: it is
  told again once the library is read.
- A long listing, a book at a time, had to end within the minute: the
  minute now counts from the last message.

`book_library_tests` failed once in the full run, an edit queued during
another not applied, and not in fifteen runs alone: the test now says why
a save is refused, for the next time.

### 2026-10-22 — goal 20, part 2: the metadata in the files (`exp-epubmeta`)

Calibre sends a copy of the book with the library's metadata written into
it: a reader that reads the book's own, KOReader among them, shows what
the library says. The wireless golden test showed it: the EPUB Calibre
sent had its package rewritten and the library's cover added, at
`OEBPS/OEBPS/cover.jpg` (Calibre joins the package's folder twice, a bug
kept for the files to be alike).

`EpubMetadata` ports both of Calibre's writers. For EPUB 3, `opf3.py`'s
`apply_metadata()`: refines, prefixes, identifiers by scheme, the main
title and its sort, authors with their roles, languages normalized with
the book's countries, series as collections, custom columns as one JSON,
the cover found or added, then `pretty_opf()` and `pretty_xml_tree()`. For
EPUB 2 and Calibre's own conversions, `opf2.py`'s OPF class: its fields by
regular expressions on the elements' names, `smart_update()` with its
quirks (the uuid written as the calibre identifier, the whole author sort
on the first author, ratings as "8.0"), the custom columns merged as
`Metadata.smart_update()` merges them, `render()`. Nine packages, three of
them EPUB 2, with three sets of metadata each, come out byte for byte as
Calibre writes them (`tools/oracle/epub_meta_oracle.py`). Getting there
meant writing XML as lxml does: Qt's writer escapes quotes in text, lxml
does not; and lxml declares, on an element made with a namespace map, only
the namespace the element is in.

The XML tree and the ZIP code of the KEPUB port moved to `xml_tree` and
`book_zip`, shared. Sends to apps write the metadata into EPUB, AZW3 and
MOBI copies, and the wireless golden test compares the files sent entry by
entry; plain EPUBs sent to a Kobo get it too.

### 2026-10-22 — goal 21, the library's highlights (`exp-highlights`)

`AnnotationSearch` ports Calibre's `all_annotations()` and
`search_annotations()`: the newest first, or the best first of a search of
its full text index, stemmed or not, with snippets whose words found are
marked; `sorted_items()`, each book's in their order by their CFIs; and
the text and Markdown export of its "Browse annotations", chapters as
headings. Calibre's own API saved highlights and bookmarks in a copy of the
fixture (`tools/oracle/annotations_oracle.py`), and ten searches find what
Calibre finds, in its order, with its snippets: Calibre's tokenizer,
ported in `exp-fts`, makes the same index terms of the query.

The Highlights section shows them by book, with their colours, notes,
chapters and dates; its search and a filter by kind; Enter or Read opens
the book in the reader where the highlight is (`ReaderWindow.openAt()`);
Copy and Export give Calibre's export. The library counts them in the
sidebar.

Left: the notes the readers keep (Kobo, Kindle), which show on each book's
page; editing a highlight's note from here.


### 2026-10-22 — a review of goal 20, part 2 (`exp-review12`)

The ZIP written (mimetype first and stored), obfuscated fonts (the package's
identifier is never changed), covers by `../` or `%20` and the KEPUB code
under the new serializer came out right. Three bugs, fixed, and a minor one:

- A series Calibre wrote freed its refinements while the loop still read
  them (AddressSanitizer saw it on the tests' own package); removed nodes
  now stay alive until the package is written, as lxml keeps them.
- A package in Latin-1 or another encoding its declaration names was read
  as UTF-8, its other fields and its non-ASCII addresses mangled in the
  book sent; the declaration is honoured, as `xml_to_unicode()` does.
- A package with a bare `&` or an undeclared `opf:` prefix, which Calibre's
  parser recovers from, got no metadata; it is repaired and read again.
- A book that cannot be read as an EPUB goes as it is, as Calibre sends a
  book its writer fails on, and an entry that cannot be read fails the book
  rather than go missing from it; a send whose copy cannot be made fails,
  rather than tell the books sent.

### 2026-10-23 — goals 15 and 20 reverted, and a cleanup (`exp-revert`)

The user asked for goals 6, 8, 15 and 20 to be reverted, then for the whole
code to be cleaned. Asked what the reverts would take with them, the user
kept goal 6 (goal 17's Kindle clippings show only through it) and goal 8
(the native sends to a Kobo and a Kindle make their paths with the template
engine), and reverted goal 20 whole, its EPUB metadata writer included.

- Goal 20 reverted (`exp-wireless`, `exp-review11`, `exp-epubmeta`,
  `exp-review12`): Calibre's wireless device server, the sends to reader
  apps, the EPUB metadata writer with `xml_tree` and `book_zip`. A KEPUB
  gets the library's metadata from its own code again, and a plain EPUB
  goes to a Kobo as a copy, as before goal 20.
  `CalibreLanguages::Names::translate()` stays, as the highlights use it.
- Goal 15 reverted (`exp-server`, `exp-review6`): the OPDS catalog and web
  page, the HTTP server, the QR codes, Settings' Network section and
  `cousoir-cli serve`. Two DOC.md fixes the merge carried, on what needs
  Calibre and `cousoir-cli kepubify`, and a check of `editsBooks()` that
  goal 20's review added, are back.
- The 23 tests pass after the reverts.

The cleanup, by five agents each on its own files, then their findings
across files:

- Dead code gone: functions, members, properties and map keys nothing read
  (`CalibreSearch::locations()`, the search index's tag, series and
  publisher rows, `Names::fromCalibre()`, `NativeSend::shortenComponents()`,
  `BookReader::readableFormat()`, `DeviceBook::authorSort`, the Kindle
  clippings' author sort, Kepub's language option, unused fields of the
  EPUB reader, MOBI writer and annotation search, Theme's dark flags and
  the `dark` properties of `SystemAppearance` and `OmarchyTheme`, the
  `available` properties of the devices, conversions and downloads, which
  had been always true since Cousoir sends, converts to KEPUB and downloads
  without Calibre); unused includes, ids, QML functions, an icon and
  reader.js exports.
- Duplicates made one: `fmt_sidx()` (`CalibreText::seriesIndexText()`),
  `format_abspath()` (`CalibreDb::formatFile()`), the file size and month
  texts (`BookDisplay`), the KEPUB options (`Kepub::optionsFor()`), the
  format a native send picks (`NativeSend::sendFormat()`), the list's known
  columns, a book's cover bytes, the series number parsing of the edits,
  two template functions' helpers, and the tests' shared helpers
  (`test_support.h`).
- Small bugs found on the way, fixed: the palette's Highlights section
  focused the hidden books list; Convert to AZW3 was offered in the menu and
  by Ctrl+Shift+A without Calibre, which refused it; a PDF closed kept its
  title; the command line's help left out `kepubify` and Kindles.
- Not changed, noted: a date search takes ISO and numeric dates only, where
  Calibre's `parse_date()` reads "apr 2015" too.

### 2026-10-23 — README and CREDITS brought up to date (`exp-credits`)

- CREDITS: `fmt_sidx()` now credited to `calibre_text.cpp`, where the
  cleanup moved it, and `format_abspath()` only to `calibre_db.cpp`; the
  bulk edit's port (`book_edit.cpp`), never listed, has its row; a few
  functions the files' headers name were missing from their rows. Calibre
  at run time also downloads metadata and names languages. The headers of
  `opf.cpp`, `book_list_model.cpp` and `calibre_text.cpp` follow the move.
- README: converting to KEPUB and the native sends work without Calibre,
  Calibre's converters, drivers and sources run when it is installed;
  Ctrl+Shift+O, reading in Cousoir, is in the shortcuts (and DOC.md's);
  Built on Calibre names the search, template and sort ports, the viewer's
  positions and highlights, the notes, kepubify and the sends; ICU,
  Snowball, xxHash and Grant Drake's Find Duplicates are thanked.

### 2026-10-25 — round 1, R1: a page made for reading (`exp-page`)

Tried on three books of Project Gutenberg in a sandbox library, by
screenshots of the reader (`cousoir-benchmark --screenshot --read`, which
now takes the reader's size, its settings and clicks on its controls).

- The page: `ReaderWindow` centres the page at the measure picked (66
  characters of half the text's 16 pixels each by default), the margins
  beside it in the page's colours, and asks `reader.js` for one or two
  columns a screen: two when the window holds two of 85 % of the measure.
  A column is the screen's width over their count less the margins, the
  gap twice the margin, so that a screen is still a page to turn. A click
  or the wheel beside the page turns it too.
- A scroll: the same page without columns, scrolled; a "page" is a window
  high, for the footer and the table of contents; Space goes a window less
  two lines down; the wheel at the end of a chapter goes on to the next once
  it stayed there a moment.
- The place through layouts: the reader's script keeps an anchor, the place
  gone to or the start of the page turned to, and every new layout (a
  setting, a size, the font) goes back to it, not to the start of the page
  it lands on, which would creep back a little with each one. Scrolls the
  script makes itself do not move the anchor.
- The type: a font family over the book's (code kept monospace), a line
  height, and an alignment for the paragraphs the book aligns to the start
  or justifies, marked once before any rule of the reader's applies, so
  that centred ones stay centred. The size stays WebEngine's zoom, which
  scales a book's pixel sizes as its relative ones.
- The page's colours: Cousoir's theme, light, sepia, dark, black; the
  highlights' colours follow the page's lightness.
- The settings, in a panel under an Aa button, are kept for every book
  (`Reader/` settings). ChoiceBox's list now scrolls past a dozen rows,
  for the fonts.
- The footer: the chapter, the page in it, the time left in the chapter
  and the book at the reader's pace (bytes of the book's files a second,
  measured from the pages turned forward after 3 s to 5 min, 30 until then:
  250 words a minute of Gutenberg's XHTML), how far into the book, and a
  bar a click on which goes there (`BookReader::locate()`).
- F11 full screen, the toolbar sliding over the page with the mouse at the
  top or after a click in the middle; Esc leaves full screen first.
- Fixed on the way: in a scroll, `line-height: normal` made the step NaN.
- Tests: `laysOutPagesAsAsked` (a scroll and back, a larger size and back,
  two pages in a wide window, one asked for, each on the same page; the
  type and the page's colours read back from the page; the settings kept;
  a place near the end by the progress bar; the time left told). The UI
  tests pass at both scales.

### 2026-10-25 — round 2, R2: find in the book (`exp-search`)

- `BookSearch` searches an EPUB's spine items as the reader shows their
  text: `EpubCfi::PageText`, the Kobo placement's walk of a page's body
  moved to be shared, white space as one space, the characters that show
  nothing left out, and here a space between blocks the page has none
  between ("Suite</h2><p>Une"). Each character is folded as the library's
  search folds (`CalibreText::foldForSearch()`, so "œ" is "oe"), the folded
  text keeping for each of its characters the page's one, so that a place
  found is a range of the page's text, and its CFIs are `EpubCfi`'s, which
  `encodesCfisAsTheReader` checks against the reader's script. The folded
  text of each spine item is kept; a page is parsed again only when it has
  places. On Pride and Prejudice (Gutenberg, 24 MB with its plates): 34 ms
  the first search, under 1 ms the next.
- `BookReader::search()` runs it on a thread; a newer search or another
  book makes an answer stale. The results carry their chapter, from
  `HighlightJson::tocFamily()`, and whether each is the first of one.
- The Search tab (Ctrl+F): the places under their chapters, the words
  found in bold; the first from the page read on shown (the page of each
  place of the spine item asked of the script), F3 and Shift+F3 through
  them. The page marks them with the CSS Custom Highlight API, which adds
  no node to the page: highlights' wrappers would shift the CFIs.
- Back and Forward: a link, a search's place, an entry of the contents or
  the highlights, or the progress bar keeps where reading was.
- Bugs found on the way, fixed:
  - A link to another chapter crashed Cousoir, since goal 19: the page was
    loaded from within `onNavigationRequested`, which fails a check of
    Chromium's (SIGTRAP in `WebContentsAdapter::load`). It is loaded once
    the handler returns. No test had followed a link to another file.
  - The reader's Ctrl+F was the library's: a window declared in the
    library's makes it its transient parent, and Qt counts the library's
    window active while the reader is, so both windows' Ctrl+F matched and
    neither ran. The reader and the PDF reader are windows of their own.
  - A shortcut naming F3 twice (`StandardKey.FindNext` and "F3") is
    ambiguous with itself and does nothing.
  - While the page has the focus, Qt WebEngine takes the keys before the
    window's shortcuts: Ctrl+F, F3, Alt+Left and Alt+Right are also read
    from the page's key events.
- Tests: `searchesTheText` (accents, ligatures, blocks, elements, white
  space, soft hyphens, chapters, CFIs naming the text, the limit) and the
  UI's `searchesTheBook` (the places, the first shown and marked, F3, Back
  and Forward, a link to a note and back). All 26 tests pass, the golden
  test included.

