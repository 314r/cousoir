# Reliure

A Qt/QML ebook manager that works on a Calibre library. This first version
only reads it: the books, sorted and searched as a table or as covers, the
authors, series, tags, languages and publishers that browse them, and each
book's metadata, description and files, which open in the desktop's reader.

Calibre stays the library's owner and keeps working as before. Reliure reads
Calibre's own `metadata.db` and follows Calibre's rules wherever it shows
something Calibre computes, so both always agree. Editing, adding books,
conversion and sending to a Kobo or a Kindle come in the next versions.

The project structure, theming (system, light, dark and Omarchy) and Settings
window come from the JolieBulle Qt port, which took them from kbextract.

## Interface

The interface follows the Omarchy style: the system monospace font, square
corners, hairline dividers, foreground-tinted hover and selection states
instead of accent fills, and one accent per screen.

The sidebar has the books, then the browse sections with their sizes:
authors, series, tags, languages and publishers. A section lists its items
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
the book, opens it in the reader or shows its folder.

When the library could not be written right now, the status bar says why
with a lock: Calibre open on this computer, Calibre writing, Dropbox syncing
or not running, a change from elsewhere in the last two minutes, or a Dropbox
conflict copy of the database. This version never writes, but the same checks
will gate editing. A click on the lock opens the library settings.

## Settings

Settings has two sections. Appearance picks the theme and the text size, as
in JolieBulle. Library shows the library folder, lets you pick another one
(Reliure remembers it) or go back to the library Calibre opens, and lists the
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
```

`list` prints the books exactly as `calibredb list --for-machine --fields
all` does, byte for byte. `doctor` reports the library's books and files,
the problems it finds (a file or folder the database lists but the disk does
not have, a cover Calibre does not list, a conflict copy, an unknown
structure) and the checks that would keep it read-only. It exits with 1 when
it finds a problem. Both read a copy of the database in a temporary folder.

## Keyboard

| Shortcut | Action |
|---|---|
| Up/Down, arrows in the grid | Move in the list |
| Enter, Space, double click | Open the book, or the section's item |
| Menu, Shift+F10, right click | Book actions: open, open in reader, show folder |
| Ctrl+O | Open the book, or the selected one, in the reader |
| Ctrl+G | Show the books as covers or as a table |
| Esc, Alt+Left | Back up one level |
| Ctrl+F | Search |
| F5, Ctrl+R | Reload the library |
| Ctrl+I | Show or hide the book details pane |
| Ctrl++, Ctrl+-, Ctrl+0 | Text size |

## Requirements

- Qt 6.9 or later with QML, Quick, Quick Controls 2, SVG, Network and
  Concurrent
- SQLite 3.31 or later
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

- `calibre_text_tests` compares the title and author sorts with what Calibre
  9.16 computes for the same names in `tests/data/oracle/text.json`, written
  by `make oracle` (`tools/oracle/text_oracle.py` run by `calibre-debug`).
- `tests/data/library` is a six-book library made by `calibredb`, with
  `tests/data/library-list.json`, its `calibredb list` output; `make fixtures`
  (`tools/make_fixture_library.sh`) builds both again. `calibre_db_tests`
  reads it, checks Reliure's list against Calibre's byte for byte, and checks
  that reading leaves every file of the library as it was.
- `golden_list` runs `calibredb list` and `reliure-cli list` on separate
  copies of the fixture and compares them; with `RELIURE_GOLDEN_REAL=1` it
  does the same with the library Calibre opens. It is skipped without
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
