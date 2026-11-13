---
name: cousoir
description: Read, search, check and change a Calibre ebook library from the command line with cousoir-cli, without Calibre and without a display. Use when asked to list or find books in a Calibre library, edit their metadata, add or remove books and formats, check a library's health, convert an EPUB to KEPUB or AZW3 (or a Kindle book to EPUB), or read what is on a Kobo or a Kindle.
---

# Cousoir's command line

`cousoir-cli` works on the library Calibre opens on this computer, or the one
given with `--library PATH`. It needs no display. It reads a copy of
`metadata.db`, so reading is always safe, Calibre open or not.

Every command prints to stdout and reports errors on stderr. Exit status: 0
when it worked, 1 when it failed or found a problem, 2 when the command line
or the library could not be read. Output is JSON except where noted.

## Reading

```bash
cousoir-cli list                       # every book, as `calibredb list --for-machine --fields all`
cousoir-cli search tag:sf 'rating:>=4' # ids found, comma-separated, as `calibredb search`; --limit N
cousoir-cli doctor --json              # books, files, problems, and whether the library could be written now
cousoir-cli read book.epub …           # the metadata adding these files would read
cousoir-cli template templates.json    # what each Calibre template of a JSON array makes of each book
cousoir-cli download 12 --covers       # what the web finds for book 12, and what it would change
cousoir-cli device /run/media/$USER/KOBOeReader   # the books on a Kobo; `mtp` or `mtp:SERIAL` for a Kindle
cousoir-cli kobo-highlights /run/media/$USER/KOBOeReader   # a Kobo's highlights and notes, or a KoboReader.sqlite
cousoir-cli search-text --json 'jazz AND cocktail'   # books whose text has the words: id, format, passage
```

`search-text` reads Calibre's full text index; `cousoir-cli index-text`
makes it and indexes the books (it writes, so the rules of `apply` hold).

`search` takes Calibre's search language (`author:vian not tag:read`,
`title:"=Mood Indigo"`, `series:true`, `#genre:sf`). It exits with 1 when
nothing matches.

## Changing the library

```bash
cousoir-cli apply changes.json
```

`changes.json` is an array, applied as the application applies them, with
the same journal, backup and checks:

```json
[{ "book": 1, "set": { "title": "Mood Indigo", "tags": ["Roman", "Jazz"], "rating": 8 } },
 { "add": ["book.epub", "book.pdf"] },
 { "addFormat": { "book": 1, "file": "book.mobi" } },
 { "removeFormats": { "book": 2, "formats": ["AZW3"] } },
 { "trash": [4] },
 { "restore": 4 }]
```

`set` takes `title`, `sort`, `authors`, `authorSort`, `series`,
`seriesIndex`, `tags`, `rating` (0 to 10), `publisher`, `languages`,
`identifiers`, `pubdate`, `comments`, `coverFile`, `removeCover`, and custom
columns by their search term (`"#genre": "SF"`). Files are relative to the
JSON file. `cousoir-cli --help` and Cousoir's DOC.md list the other changes
(bulk edits, conversions, sending to a reader).

Rules:

- `apply` refuses while Calibre is open, or while a sync service or another
  computer may be changing the library. Run `cousoir-cli doctor --json` and
  read `writing.state` to see why; ask the user to close Calibre rather than
  working around it.
- Never pass `--force` unless the user asks for it: it skips the sync and
  conflict checks that keep a shared library from being damaged.
- Nothing is deleted for good: removed books and files go to the library's
  trash, and `restore` brings them back.
- Check the result with `list` or `search` after a change.

## Converting, without Calibre

```bash
cousoir-cli kepubify book.epub book.kepub   # for a Kobo
cousoir-cli azw3 book.epub book.azw3        # for a Kindle
cousoir-cli epub book.azw3 book.epub        # a Kindle book (AZW3, MOBI) as an EPUB
```

These print nothing when they work. The original file is never changed.
