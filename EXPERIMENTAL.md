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
| 1. A large library stays instant | next |
| 2. Search like Calibre | planned |
| 3. Many books at once | planned |
| 4. Editing the description | planned |
| 5. Custom columns | planned |
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
