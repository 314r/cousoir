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
  Calibre's translations of "Unknown" and "Cover", the MOBI language codes,
  the HTML entities, and how Calibre's ICU collators match text in a search
  are generated from Calibre itself.
- **Calibre itself, at run time.** When Calibre is installed, Cousoir runs
  it, through `calibre-debug`, to convert books; to send them to a Kobo or a
  Kindle with Calibre's own drivers; to download metadata and covers from
  Calibre's sources; to read the formats it has no reader for, in its
  library and in its own reader, FB2, DOCX, RTF, TXT, comics and others made
  EPUBs by Calibre's `ebook-convert`; and to draw
  the cover of an EPUB from its first page. It names languages from
  Calibre's ISO 639 table and translations, read from the Calibre installed.
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
| `calibre_text.cpp` | `src/calibre/ebooks/metadata/__init__.py`: `title_sort()`, `get_title_sort_pat()`, `quote_pairs`, `author_to_author_sort()`, `remove_bracketed_text()`, `string_to_authors()`, `authors_to_string()`, `check_isbn()`, `check_doi()`, `fmt_sidx()` |
| | `resources/default_tweaks.py`: the title sort articles of every language and the author sort word lists |
| | `src/calibre/__init__.py`: `sanitize_file_name()` |
| | `src/calibre/utils/filenames.py`: `ascii_filename()` |
| | `src/calibre/ebooks/html_entities.c`: `replace_entities()` |
| | `src/calibre/db/__init__.py`: `_get_next_series_num_for_list()` |
| | `src/calibre/db/utils.py`: `fuzzy_title()` |
| | `src/calibre/utils/localization.py`: `canonicalize_lang()` |
| | `src/calibre/utils/cleantext.py`: `clean_xml_chars()`, `clean_ascii_chars()` |
| `calibre_paths.cpp` | `src/calibre/db/backend.py`: `construct_path_name()`, `construct_file_name()`, `WINDOWS_RESERVED_NAMES` |
| `calibre_languages.cpp` | `src/calibre/utils/localization.py`: `calibre_langcode_to_name()`, `canonicalize_lang()`, `lang_map()`, `translator_for_lang()` and its fallbacks, `get_lc_messages_path()`, `locale_fallbacks()`, `available_translations()` |
| `kepub.cpp` | `src/calibre/ebooks/oeb/polish/kepubify.py`: `kepubify_container()`, `add_kobo_spans()`, `wrap_body_contents()`, `add_style_and_script()`, `remove_kobo_markup_from_html()`, `add_dummy_title_page()`, `is_probably_a_title_page()`; `resources/templates/kobo.js` (in `assets/kobo`) |
| `kobo_send.cpp` (without Calibre) | `src/calibre/devices/utils.py`: `create_upload_path()`; `src/calibre/library/save_to_disk.py`: `get_components()`, `get_component_metadata()`; `src/calibre/utils/filenames.py`: `shorten_components_to()`, `shorten_component()`, `limit_component()`; `src/calibre/devices/kobo/driver.py`: KoboTouch's `MAX_PATH_LEN`, `sanitize_path_components()`, `filename_callback()`, its settings; `src/calibre/ebooks/metadata/book/json_codec.py`: the book's entry in metadata.calibre; `src/calibre/devices/usbms/device.py`: driveinfo.calibre |
| `open_library.cpp` | `src/calibre/ebooks/metadata/sources/base.py`: `Source.get_title_tokens()`, `Source.get_author_tokens()`, `cleanup_title()`, `InternalMetadataCompareKeyGen`; `src/calibre/ebooks/metadata/sources/prefs.py`: the defaults of `msprefs`; the fallback from an ISBN to a title search, after `src/calibre/ebooks/metadata/sources/google.py` |
| `mobi_book.cpp` | `src/calibre/ebooks/mobi/reader/headers.py`: `EXTHHeader`, `BookHeader`; `mobi6.py`: `MobiReader`'s sections, `check_for_drm()`, `sizeof_trailing_entries()`, `extract_text()`, `add_anchors()`, `cleanup_html()`, `replace_page_breaks()`, `extract_images()`, `upshift_markup()` (links, ids, pictures), `create_opf()` (the inline contents); `mobi8.py`: `Mobi8Reader`'s `read_indices()`, `build_parts()`, `get_file_info()`, `get_id_tag_by_pos_fid()`, `get_id_tag()`, `create_ncx()`, `extract_resources()`; `index.py`: `read_index()`, `parse_indx_header()`, `CNCX`, `parse_tagx_section()`, `get_tag_map()`, `parse_index_record()`; `markup.py`: `expand_mobi8_markup()`; `ncx.py`: `read_ncx()`, `build_toc()`; `containers.py`: `Container`; `src/calibre/ebooks/mobi/huffcdic.py`: `HuffReader`; `src/calibre/ebooks/compression/palmdoc.c`: `cpalmdoc_decompress()`; `src/calibre/ebooks/mobi/utils.py`: `decint()`, `decode_string()`, `read_font_record()` |
| `mobi_metadata.cpp` | `src/calibre/ebooks/metadata/mobi.py`: `MetadataUpdater`; `src/calibre/ebooks/mobi/utils.py`: `rescale_image()`; `src/calibre/ebooks/mobi/reader/headers.py`: the ASIN, cdetype and MOBI version; `src/calibre/devices/kindle/driver.py`: `thumbnail_filename()`; `src/calibre/devices/kindle/apnx.py`, `apnx_page_generator/` (fast generator, pages, page groups), by John Schember and Vaso Peras-Likodric |
| `calibre_text.cpp`, `calibre_strings.inc` | `src/calibre/ebooks/mobi/langcodes.py`: `iana2mobi()` and its table; `src/calibre/utils/localization.py`: `lang_as_iso639_1()` |
| `native_send.cpp` | `src/calibre/devices/utils.py`: `create_upload_path()`; `src/calibre/library/save_to_disk.py`: `get_components()`, `get_component_metadata()`; `src/calibre/utils/filenames.py`: `shorten_components_to()`; `src/calibre/ebooks/metadata/book/json_codec.py`: a book's entry |
| `kindle_send.cpp` (without Calibre) | `src/calibre/devices/mtp/driver.py`: `get_pref()`, `prefix_for_location()`, `create_upload_path()`, `ensure_parent()`, `upload_books()`, `upload_kindle_thumbnail()`, `upload_apnx()`, `_update_drive_info()`; `src/calibre/devices/mtp/defaults.py`: Amazon's settings; `src/calibre/devices/kindle/driver.py`: `get_apnx_opts()` |
| `assets/reader/reader.js` | `src/pyj/read_book/cfi.pyj`: `encode()`, `decode()` and their helpers, the highlight wrappers skipped, `at_current()`'s scan from the top left, `cfi_for_selection()`; `src/pyj/read_book/iframe.pyj`: the spine step of a position; `src/pyj/read_book/annotations.pyj`: a highlight's fields, `remove_highlight()` |
| `epub_cfi.cpp` | `src/pyj/read_book/cfi.pyj`: `encode()`, `decode()`, `adjust_node_for_text_offset()`, `increment_index_for_child()`, `node_at_index()`, `node_for_path_step()`, `node_for_text_offset()`, `id_is_unique()`, `escape_for_cfi()`, `unescape_from_cfi()`, worked on a page's XML rather than the browser's document |
| `book_reader.cpp`, `book_operations.cpp` (positions and highlights), `highlight_json.cpp` | `src/calibre/db/backend.py`: `save_last_read_position_to_cursor()`, `save_annotations_for_book()`; `src/calibre/db/annotations.py`: `annot_db_data()`, `merge_annot_lists()` (the newest of an id winning, in `mergeSteps()`); `src/pyj/read_book/annotations.pyj`: a highlight's fields, `remove_highlight()`; `src/calibre/gui2/viewer/integration.py`: the viewer's user and device; `src/calibre/constants.py`: `builtin_colors_light`, `builtin_colors_dark` |
| `annotation_search.cpp`, `highlight_browser.cpp` | `src/calibre/db/backend.py`: `all_annotations()`, `search_annotations()` with `restrict_to_user`, `all_annotation_users()`; `src/calibre/gui2/library/annotations.py`: `sorted_items()`, `ChapterGroup`, `render_highlight_as_text()`, `render_bookmark_as_text()`, `render_timestamp()`, the export's `exported_data()`, `AnnotsResultsDelegate.result_data()`; `src/calibre/ebooks/epub/cfi/parse.py`: `cfi_sort_key()`, its path and offsets |
| `calibre_notes.cpp`, `calibre_db.cpp` (notes) | `src/calibre/db/notes/connect.py`: `Notes.set_note()`, `set_backup_for()`, `path_to_retired_dir_for_item()`, `retire_entry()`, `remove_retired_entry()`, `trim_retired_dir()`, `hash_key()`, `notes_for()`, `get_note_data()` (read) |
| `calibre_fts.cpp` | `src/calibre/db/sqlite_extension.cpp`: the FTS5 `Tokenizer` (its UTF-8 decoder, after Björn Höhrmann's, its script blocks, break iterators, diacritics and stemmers) and its registration as `unicode61`, `calibre` and `porter`; `src/calibre/utils/localization.py`: `bcp47_locale_name()` |
| `book_sort.cpp` | `src/calibre/db/cache.py`: `multisort()`; `src/calibre/db/fields.py`: each field's `sort_keys_for_books()`, sort and default keys, `SeriesField`'s title sort in the book's language, `CompositeField`'s number, date, bool and list keys; `src/calibre/db/view.py`: `add_to_sort_history()`; `src/calibre/gui2/library/views.py`: the list's saved state, `cleanup_sort_history()` and `apply_sort_history()` |
| `calibre_search.cpp` | `src/calibre/utils/search_query_parser.py`: `Parser`, `SearchQueryParser`'s evaluation, saved searches |
| | `src/calibre/db/search.py`: `_matchkind()`, `_match()`, `DateSearch`, `NumericSearch`, `KeyPairSearch`, `Parser.get_matches()`, `get_user_category_matches()`, `Search._do_search()` |
| | `src/calibre/db/fields.py`: the values each field searches, custom columns' included, `iter_searchable_values_for_sort()` |
| | `src/calibre/library/field_metadata.py`: the fields' search terms and data types |
| `calibre_template.cpp`, `calibre_template_engine.cpp` | `src/calibre/utils/formatter.py`: the template language's lexer (`cached_lex_scanner()`), `_Parser`, `_Interpreter`, `TemplateFormatter` with `_do_format()` and `format_field()`; `src/calibre/ebooks/metadata/book/formatter.py`: `SafeFormat`; `src/calibre/__init__.py`: `human_readable()` |
| `calibre_template_book.cpp` | `src/calibre/db/lazy.py`: `ProxyMetadata` and its getters; `src/calibre/ebooks/metadata/book/base.py`: `format_field_extended()`, `format_series_index()`; `src/calibre/ebooks/metadata/book/formatter.py`: `SafeFormat.get_value()`; `src/calibre/ebooks/metadata/book/__init__.py`: `ALL_METADATA_FIELDS`; `src/calibre/utils/date.py`: `format_date()`, `parse_date()` with the forms of dateutil's parser it takes; `src/calibre/ebooks/metadata/__init__.py`: `fmt_sidx()` |
| `calibre_template_functions.cpp` | `src/calibre/utils/formatter_functions.py`: the builtin template functions; `src/calibre/utils/titlecase.py`, MIT licence, John Gruber, Stuart Colville and Kovid Goyal: `titlecase()`; `src/calibre/utils/icu.py`: `capitalize()`; `src/calibre/ebooks/metadata/__init__.py`: `rating_to_stars()`; `src/calibre/ebooks/metadata/search_internet.py`: `qquote()`; `src/calibre/gui2/library/bookshelf_view.py`: `width_from_pages()`, `base_log()` |
| `identifier_links.cpp` | `src/calibre/ebooks/metadata/sources/identify.py`: `urls_from_identifiers()`; `src/calibre/ebooks/metadata/sources/google.py`, `amazon.py` and `edelweiss.py`: `get_book_url()`, `get_book_url_name()`, Amazon's `get_domain_and_asin()` and `referrer_for_domain()`; `src/calibre/ebooks/metadata/__init__.py`: `check_issn()` |
| `opf_reader.cpp` | `src/calibre/ebooks/metadata/opf3.py`: `read_metadata()` and the readers it calls, `parse_prefixes()`, `expand_prefix()`, `parse_identifier()`, `find_main_title()` |
| | `src/calibre/ebooks/metadata/opf2.py`: the `OPF` class's readers, `first_spine_item()`, the EPUB 3 raster cover |
| | `src/calibre/ebooks/metadata/utils.py`: `parse_opf_version()` |
| | `src/calibre/ebooks/metadata/book/base.py`: `set_identifier()`, `set_user_metadata()`, `smart_update()`'s custom fields |
| | `src/calibre/ebooks/metadata/book/json_codec.py`: `decode_is_multiple()` |
| | `src/calibre/utils/date.py`: `fix_only_date()` |
| `opf.cpp` | `src/calibre/ebooks/metadata/opf2.py`: `metadata_to_opf()`, `serialize_user_metadata()`; `src/calibre/ebooks/metadata/book/json_codec.py`: `encode_is_multiple()`; `src/calibre/library/field_metadata.py`: `add_custom_field()` |
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
| | `src/calibre/db/cache.py`: `create_book_entry()`, `set_metadata()`, `_do_add_format()`, `remove_formats()`, `remove_books()`, `move_book_from_trash()`, `move_format_from_trash()`, `author_sort_from_authors()`, `get_next_series_num_for()`, `_add_newbook_tag()`, `_add_default_custom_column_values()` |
| | `src/calibre/db/backend.py`: `update_path()`, `add_format()`, `remove_formats()`, `remove_books()`, `move_book_to_trash()`, `list_trash_entries()`, `get_metadata_for_trash_book()` |
| | `src/calibre/db/tables.py`: `update_fmt()` |
| | `src/calibre/db/write.py`: how the adapters take each kind of value |
| `book_edit.cpp` | `src/calibre/gui2/dialogs/metadata_bulk.py`: `MyBlockingBusy.do_all()`, the bulk edit; `src/calibre/db/legacy.py`: `cleanup_tags()`, `_do_bulk_modify()`; `src/calibre/db/cache.py`: `get_next_series_num_for()` |
| `field_writers.cpp` | `src/calibre/db/write.py`: the field writers, `one_one_in_books()`, `one_one_in_other()`, `many_one()`, `many_many()`, `identifiers()`, `custom_series_index()`, with `get_db_id()`, `change_case()`, `get_series_values()`, `clean_identifier()`, `get_adapter()` and the `adapt_*()` functions; `Writer.set_books_for_enum()` |
| | `src/calibre/db/fields.py`: `author_sort_for_book()` |
| | `src/calibre/db/cache.py`: `set_field()`, `set_metadata()` |
| `commit_engine.cpp` | `src/calibre/__init__.py`: `fit_image()`; `src/calibre/utils/img.py`: `save_cover_data_to()` |
| `calibre_db.cpp` | `src/calibre/db/backend.py`: the SQL functions, aggregates and temporary author triggers of a connection, `format_abspath()`, the custom columns `initialize_custom_columns()` keeps, with their separators |
| | `src/calibre/db/tables.py`: how the custom columns' tables are read |
| `book_library.cpp` | `src/calibre/db/utils.py`: `find_identical_books()`; `src/calibre/ebooks/__init__.py`: `BOOK_EXTENSIONS` |
| `book_list_model.cpp` | `src/calibre/ebooks/metadata/__init__.py`: `rating_to_stars()` |
| `device_books.cpp` | `src/calibre/gui2/device.py`: `set_books_in_library()` |
| `device_files.cpp` | `src/calibre/devices/mtp/driver.py`: `is_folder_ignored()` |
| `kobo.cpp` | `src/calibre/devices/kobo/driver.py`, Copyright (C) Timothy Legge, Kovid Goyal and David Forrester: the books KOBOTOUCH's `books()` lists, `imageid_from_contentid()`, `images_path()`, `cover_file_endings()`, `qhash()`, `path_from_contentid()`, the device models |
| | `src/calibre/devices/kobo/bookmark.py`, Copyright (C) Timothy Legge and Kovid Goyal: `Bookmark.get_bookmark_data()` |
| `kindle.cpp` | `src/calibre/devices/mtp/driver.py`: `_sync_kindle_thumbnails()`, `delete_kindle_cover_thumbnail_for()`; `src/calibre/devices/mtp/filesystem_cache.py` and `src/calibre/ebooks/__init__.py`: the book extensions |
| `kindle_send.cpp` | `src/calibre/devices/mtp/unix/driver.py`: `put_file()`, `get_mtp_file()`, `delete_file_or_folder()`; `src/calibre/devices/mtp/driver.py`: `open()`, `books()`, in the Python the job runs |
| `metadata_download.cpp` | `src/calibre/ebooks/metadata/sources/worker.py`: `single_identify()` and `single_covers()`, which the jobs call; `src/calibre/gui2/metadata/single_download.py`: the dialog's identify and covers workers, its order of covers; `src/calibre/gui2/metadata/single.py`: `fetch_metadata()` and `update_from_mi()`; `src/calibre/library/comments.py`: `comments_to_html()`, `merge_comments()`; `src/calibre/db/cache.py`: `author_sort_from_authors()`; `src/calibre/gui2/widgets2.py`: the rating editor's whole stars |
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
- `calibre_template.inc`: the built-in fields of Calibre's
  `FieldMetadata`, `src/calibre/library/field_metadata.py`, and by
  interface language the names of days and months of Calibre's locale data
  and the few words templates print, as Calibre's translators wrote them;
  written by `tools/oracle/template_tables_oracle.py`.
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

`tools/oracle/source_oracle.py` records the words Calibre's sources
search by. The answers in `tests/data/openlibrary` are Open Library's
(openlibrary.org, whose data is under CC0).

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

How a Kobo's highlights are put in chapters and written out as Markdown,
Obsidian's Markdown or plain text comes from
[kbextract2](https://github.com/314r/kbextract2), Copyright (c) 2026 314r,
MIT license: `kobo_highlights.cpp`'s chapters, from its
`internal/kobo/reader.go` and `paths.go`; `highlight_formats.cpp`, from
`internal/kobo/format.go` and `text.go`; and the fixture
`tests/data/kobo-kbextract.sql`, its `testdata/fixture.sql`. They keep the
MIT notice too.

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
[SQLite](https://sqlite.org), [libzip](https://libzip.org),
[ICU](https://icu.unicode.org), [Snowball](https://snowballstem.org)'s
libstemmer, [xxHash](https://xxhash.com) and
[libmtp](https://libmtp.sourceforge.net), and runs poppler's `pdfinfo` and
`pdftoppm` to read PDF files when Calibre is not installed.
