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
| 5. Custom columns | next |
| 6. Reading life from the readers | planned |
| 7. A command palette | planned |

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
