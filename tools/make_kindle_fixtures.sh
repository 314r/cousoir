#!/usr/bin/env bash
# Writes the Kindle books of the reader's voyage, made by Calibre of its
# EPUB: an AZW3, a MOBI holding both kinds, and a MOBI of the old kind alone,
# Calibre's MOBI output by default, for the reader's Kindle reading.
#
#   tools/sandbox.sh tools/make_kindle_fixtures.sh
set -euo pipefail

cd "$(dirname "$0")/.."
ebook-convert tests/data/reader/voyage.epub tests/data/reader/voyage.azw3
ebook-convert tests/data/reader/voyage.epub tests/data/reader/voyage.mobi --mobi-file-type both
ebook-convert tests/data/reader/voyage.epub tests/data/reader/voyage-old.mobi --mobi-file-type old
