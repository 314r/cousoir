# Credits

Cousoir stands on other people's work, Calibre's most of all. This file says
whose, and where it is.

## Calibre

[Calibre](https://calibre-ebook.com) is the free and open-source ebook manager
created by Kovid Goyal, Copyright (C) 2008-2026 Kovid Goyal and the Calibre
contributors, released under the GNU General Public License version 3. Its
source is at <https://github.com/kovidgoyal/calibre>.

Cousoir works on Calibre's libraries, and could not do so without Calibre:

- **Its code.** Wherever Cousoir computes something Calibre computes, it
  follows Calibre's own code, ported from Python to C++ from Calibre 9.16.
  This is why Cousoir is released under the GPL version 3 too. The files that
  hold ported code say so at their top, and are listed below.
- **Its data.** The transliteration table that turns names into folder names,
  Calibre's translations of "Unknown" and "Cover", the MOBI language codes and
  the HTML entities are generated from Calibre itself.
- **Calibre itself, at run time.** Cousoir runs Calibre, through
  `calibre-debug`, to convert books and to send them to a Kobo or a Kindle
  with Calibre's own drivers; to read the formats it has no reader for; and to
  draw the cover of an EPUB from its first page.
- **Its formats.** `metadata.db`, the `metadata.opf` files, the library's
  folder layout, its `.caltrash` trash, `metadata.calibre` and
  `driveinfo.calibre` on a reader, and Calibre's settings files are Calibre's
  designs, which Cousoir reads and writes as they are.
- **Its tests.** Cousoir's tests run Calibre on the same libraries and the
  same books, and compare every result with Calibre's.

Cousoir is an independent project: it is not made, endorsed or supported by
the Calibre project. Please report Cousoir's bugs here, not to Calibre.

### Calibre's code in Cousoir

Each Cousoir file below holds code ported from the Calibre source files named
beside it. Unless stated otherwise those files are Copyright (C) Kovid Goyal,
GPL v3, as their own headers and Calibre's `COPYRIGHT` say.

| Cousoir | Calibre |
|---|---|
| `calibre_text.cpp` | `src/calibre/ebooks/metadata/__init__.py`: `title_sort()`, `get_title_sort_pat()`, `quote_pairs`, `author_to_author_sort()`, `remove_bracketed_text()`, `string_to_authors()`, `authors_to_string()`, `check_isbn()`, `check_doi()` |
| | `resources/default_tweaks.py`: the title sort articles of every language and the author sort word lists |
| | `src/calibre/__init__.py`: `sanitize_file_name()` |
| | `src/calibre/utils/filenames.py`: `ascii_filename()` |
| | `src/calibre/ebooks/html_entities.c`: `replace_entities()` |
| | `src/calibre/db/__init__.py`: `_get_next_series_num_for_list()` |
| | `src/calibre/db/utils.py`: `fuzzy_title()` |
| | `src/calibre/utils/localization.py`: `canonicalize_lang()` |
| | `src/calibre/utils/cleantext.py`: `clean_xml_chars()`, `clean_ascii_chars()` |
| `calibre_paths.cpp` | `src/calibre/db/backend.py`: `construct_path_name()`, `construct_file_name()`, `WINDOWS_RESERVED_NAMES` |
| `opf_reader.cpp` | `src/calibre/ebooks/metadata/opf3.py`: `read_metadata()` and the readers it calls, `parse_prefixes()`, `expand_prefix()`, `parse_identifier()`, `find_main_title()` |
| | `src/calibre/ebooks/metadata/opf2.py`: the `OPF` class's readers, `first_spine_item()`, the EPUB 3 raster cover |
| | `src/calibre/ebooks/metadata/utils.py`: `parse_opf_version()` |
| | `src/calibre/ebooks/metadata/book/base.py`: `set_identifier()` |
| | `src/calibre/utils/date.py`: `fix_only_date()` |
| `opf.cpp` | `src/calibre/ebooks/metadata/opf2.py`: `metadata_to_opf()`; `src/calibre/ebooks/metadata/__init__.py`: `fmt_sidx()` |
| `pdf_reader.cpp` | `src/calibre/ebooks/metadata/xmp.py`: `metadata_from_xmp_packet()`, `consolidate_metadata()` and the readers they call |
| | `src/calibre/ebooks/metadata/pdf.py`: `read_info()`, `get_metadata()` |
| | `src/calibre/ebooks/metadata/book/base.py`: `smart_update()` |
| `book_import.cpp` | `src/calibre/ebooks/mobi/reader/headers.py`: `MetadataHeader`, `BookHeader`, `EXTHHeader` |
| | `src/calibre/ebooks/metadata/mobi.py`: `get_metadata()` |
| | `src/calibre/ebooks/metadata/meta.py`: `metadata_from_filename()`, `_get_metadata()` |
| | `src/calibre/ebooks/metadata/book/base.py`: `smart_update()` |
| | `src/calibre/ebooks/metadata/epub.py`: `Encryption`, `get_metadata()`, `get_cover()` |
| `library_check.cpp` | `src/calibre/library/check_library.py`: `scan_library()`, `process_book()`, `IGNORE_AT_TOP_LEVEL`, `NORMALS`; `src/calibre/db/utils.py`: `find_identical_books()` |
| `book_operations.cpp` | `src/calibre/gui2/actions/edit_metadata.py`: `merge_books()` with `merge_only_formats`, `add_formats()` |
| | `src/calibre/db/cache.py`: `create_book_entry()`, `set_metadata()`, `_do_add_format()`, `remove_formats()`, `remove_books()`, `move_book_from_trash()`, `move_format_from_trash()`, `author_sort_from_authors()`, `get_next_series_num_for()`, `_add_newbook_tag()` |
| | `src/calibre/db/backend.py`: `update_path()`, `add_format()`, `remove_formats()`, `remove_books()`, `move_book_to_trash()`, `list_trash_entries()`, `get_metadata_for_trash_book()`, `format_abspath()` |
| | `src/calibre/db/tables.py`: `update_fmt()` |
| `field_writers.cpp` | `src/calibre/db/write.py`: the field writers, `one_one_in_books()`, `many_one()`, `many_many()`, `identifiers()`, with `get_db_id()`, `change_case()`, `get_series_values()`, `clean_identifier()` and the `adapt_*()` functions |
| | `src/calibre/db/fields.py`: `author_sort_for_book()` |
| | `src/calibre/db/cache.py`: `set_field()`, `set_metadata()` |
| | `src/calibre/utils/date.py`: `fix_only_date()` |
| `commit_engine.cpp` | `src/calibre/__init__.py`: `fit_image()`; `src/calibre/utils/img.py`: `save_cover_data_to()` |
| `calibre_db.cpp` | `src/calibre/db/backend.py`: the SQL functions, aggregates and temporary author triggers of a connection, `format_abspath()` |
| `book_library.cpp` | `src/calibre/db/utils.py`: `find_identical_books()`; `src/calibre/ebooks/__init__.py`: `BOOK_EXTENSIONS`; `src/calibre/ebooks/metadata/__init__.py`: `fmt_sidx()`, `rating_to_stars()` |
| `device_books.cpp` | `src/calibre/gui2/device.py`: `set_books_in_library()` |
| `device_files.cpp` | `src/calibre/devices/mtp/driver.py`: `is_folder_ignored()` |
| `kobo.cpp` | `src/calibre/devices/kobo/driver.py`, Copyright (C) Timothy Legge, Kovid Goyal and David Forrester: the books KOBOTOUCH's `books()` lists, `imageid_from_contentid()`, `images_path()`, `cover_file_endings()`, `qhash()`, `path_from_contentid()`, the device models |
| `kindle.cpp` | `src/calibre/devices/mtp/driver.py`: `_sync_kindle_thumbnails()`, `delete_kindle_cover_thumbnail_for()`; `src/calibre/devices/mtp/filesystem_cache.py` and `src/calibre/ebooks/__init__.py`: the book extensions |
| `kindle_send.cpp` | `src/calibre/devices/mtp/unix/driver.py`: `put_file()`, `get_mtp_file()`, `delete_file_or_folder()`; `src/calibre/devices/mtp/driver.py`: `open()`, `books()`, in the Python the job runs |
| `conversion.cpp` | `src/calibre/gui2/convert/single.py`: `setup_pipeline()`; `src/calibre/gui2/tools.py`: `convert_single_ebook()`; `src/calibre/ebooks/conversion/config.py`: `load_defaults()`; `src/calibre/db/legacy.py`: `original_fmt()`, in the Python the job runs |

`concurrency_guard.cpp` uses the name of Calibre's single-instance socket,
from `src/calibre/utils/lock.py`, to tell whether Calibre is running.

The Check section's idea of finding duplicate books and author name
variants, and of telling sure matches from likely ones, comes from the Find
Duplicates plugin for Calibre by Grant Drake (kiwidude), GPL v3,
<https://github.com/kiwidude68/calibre_plugins>. No code of it is in
Cousoir.

The headers next to these files (`calibre_text.h`, `calibre_paths.h`,
`opf_reader.h`, `opf.h`, `field_writers.h`, `book_operations.h` and the
others) name, function by function, the Calibre code each declaration
follows.

### Calibre's data in Cousoir

`make oracle` runs `tools/oracle/tables_oracle.py` in Calibre to generate:

- `calibre_unidecode.inc`, the transliteration table of Calibre's
  Unihandecode, `src/calibre/ebooks/unihandecode`, Copyright (C) 2010-2011
  Hiroshi Miura and 2009 John Schember, GPL v3. It comes from
  [Text::Unidecode](https://metacpan.org/pod/Text::Unidecode), Copyright (C)
  2001 Sean M. Burke, by way of Russell Norris's Ruby unidecode, and, for the
  Han characters, from the Unicode Consortium's Unihan database.
- `calibre_strings.inc`: "Unknown" and "Cover" as Calibre's translators
  translated them; the MOBI language codes of
  `src/calibre/ebooks/mobi/langcodes.py`; and the named character references
  of `src/calibre/ebooks/html_entities.py`, which Calibre takes from Python's
  `html.entities`.

### Calibre's code in the tests and tools

The oracle scripts in `tools/oracle` run inside `calibre-debug` and call
Calibre's own functions. A few lines in them, and in the test tool, come from
Calibre and say so:

- `tools/oracle/read_oracle.py` and `tools/oracle/apply_edits.py`: `book()`
  of `src/calibre/db/cli/cmd_add.py`; `apply_edits.py` also follows
  `merge_books()` of `src/calibre/gui2/actions/edit_metadata.py`;
- `tools/oracle/convert_books.py`: `book_converted()` of
  `src/calibre/gui2/actions/convert.py`;
- `tools/oracle/send_books.py`: the methods of
  `src/calibre/devices/mtp/unix/driver.py` and `open()` of
  `src/calibre/devices/mtp/driver.py`;
- `tests/tool/readers.cpp`: `qhash()` and `imageid_from_contentid()` of
  `src/calibre/devices/kobo/driver.py`.

The fixture library in `tests/data/library` was made by `calibredb`, and the
expected results in `tests/data/oracle` and `tests/data/library-list.json` are
Calibre's output.

### Thank you

If Cousoir is useful to you, please consider
[supporting Calibre](https://calibre-ebook.com/donate).

## kbextract, JolieBulle, omadji and Omarchy

The project structure, the theming (system, light, dark and Omarchy), the
Settings window and the shared controls come from the JolieBulle Qt port,
which took them from [kbextract](https://github.com/314r/kbextract), Copyright
(c) 2026 314r, MIT license: `Theme.qml`, `omarchy_theme.*`,
`system_appearance.*`, `AppearanceSettingsPage.qml`, `SettingsWindow.qml`, the
shared controls and the theme icons. The Omarchy theming, `omarchy_theme.*` and
the colors of `Theme.qml`, is derived from bjarneo's
[omadji](https://github.com/bjarneo/omadji), Copyright (c) 2026 bjarneo, MIT
license. They keep their MIT notice, in [`LICENSES/MIT.txt`](LICENSES/MIT.txt).

The look follows [Omarchy](https://omarchy.org)'s style, and the Omarchy theme
reads the colors of the current Omarchy theme.

## Lucide

The icons `assets/icons/theme-dark.svg` and `assets/icons/theme-light.svg` are
adapted from [Lucide](https://lucide.dev)'s moon and sun, Copyright (c) Lucide
Icons and Contributors, ISC license, the moon being derived from Cole Bemis's
Feather, MIT license. Their notice is in
[`LICENSES/ISC-lucide.txt`](LICENSES/ISC-lucide.txt).

## Libraries and tools

Cousoir is built with [Qt](https://www.qt.io) and uses
[SQLite](https://sqlite.org), [libzip](https://libzip.org) and
[libmtp](https://libmtp.sourceforge.net), and runs poppler's `pdfinfo` and
`pdftoppm` to read PDF files when Calibre is not installed.
