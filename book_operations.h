#pragma once

#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "book.h"
#include "field_writers.h"
#include "opf.h"
#include "opf_reader.h"

class FileTransaction;
class SqliteDb;

// A book to add: the metadata read from its files, and the files, one per
// format, the format being a file's extension.
struct NewBook
{
    ImportedMetadata metadata;
    QStringList files;
};

// A format added to a book from a file, replacing the book's file of that
// format if it has one.
struct NewFormat
{
    int bookId = 0;
    QString file;
    // Upper case; empty for the file's extension.
    QString format;
};

// The formats of book `from` that book `to` has no file for, copied to it.
struct FormatCopy
{
    int from = 0;
    int to = 0;
};

// Formats of a book moved to the library's trash.
struct FormatRemoval
{
    int bookId = 0;
    QStringList formats;
};

// A book back from the library's trash, or with `format`, one of its formats.
struct Restoration
{
    int bookId = 0;
    QString format;
};

// One of the library's trash entries, as Calibre lists them: a book, or the
// formats removed from a book.
struct TrashEntry
{
    int bookId = 0;
    QString title;
    QString author;
    // A removed book's cover, empty for formats.
    QString coverFile;
    QDateTime removed;
    // Upper case, for formats.
    QStringList formats;
};

// Adding, removing and restoring books and formats as Calibre 9.16 does it:
// ported from create_book_entry(), add_format(), remove_formats(),
// remove_books(), move_book_from_trash() and move_format_from_trash() of
// src/calibre/db/cache.py and the functions of db/backend.py they call. The
// database changes go through the connection, the file changes through the
// transaction's journal.
//
// Cousoir never empties the trash: Calibre deletes its entries after 14 days
// when it starts.
namespace BookOperations {

struct Context
{
    QString libraryPath;
    FieldWriters::Context fields;
    // Tags every added book gets, Calibre's new_book_tags.
    QStringList newBookTags;
    // The library before the change, for the metadata.opf of the books going
    // to the trash.
    const LibraryData *before = nullptr;
    Opf::Settings opf;
};

struct Result
{
    // The books whose metadata changed, for Calibre to write again.
    QSet<int> dirtied;
    // Books whose formats changed: last_modified only.
    QSet<int> touched;
    QList<int> added;
    QList<int> restored;
};

bool addBook(SqliteDb &db, FileTransaction &files, const NewBook &book, const Context &context,
             FieldWriters::Result *fields, Result *result, QString *error);
bool addFormat(SqliteDb &db, FileTransaction &files, const NewFormat &format, const Context &context, Result *result,
               QString *error);
// Merge only formats of Calibre's merge_books() in gui2/actions/
// edit_metadata.py: add_formats(…, replace=False) copies into the book kept
// each format of the other whose file is there and that the kept book has no
// file for.
bool copyFormats(SqliteDb &db, FileTransaction &files, const FormatCopy &copy, const Context &context, Result *result,
                 QString *error);
bool removeFormats(SqliteDb &db, FileTransaction &files, const FormatRemoval &removal, const Context &context,
                   Result *result, QString *error);
bool removeBooks(SqliteDb &db, FileTransaction &files, const QList<int> &bookIds, const Context &context,
                 Result *result, QString *error);
bool restore(SqliteDb &db, FileTransaction &files, const Restoration &restoration, const Context &context,
             FieldWriters::Result *fields, Result *result, QString *error);

// set_conversion_options() of backend.py: the settings of the book's last
// conversion, `data` as Calibre stores it.
bool setConversionOptions(SqliteDb &db, int bookId, const QByteArray &data, QString *error);
// save_last_read_position_to_cursor() of Calibre's backend: a reader's
// position in the book, in place of the one it had.
bool setReadPosition(SqliteDb &db, int bookId, const ReadPosition &position, QString *error);
// save_annotations_for_book() of Calibre's backend, for one annotation of
// its viewer: in place of the one of the same id.
bool setAnnotation(SqliteDb &db, int bookId, const Annotation &annotation, QString *error);

// update_path() of backend.py for one book: its folder and files follow its
// title and first author, in the database and on disk.
bool updatePath(SqliteDb &db, FileTransaction &files, const QString &libraryPath, int bookId, const QString &unknown,
                QString *error);

// format_abspath() without its rename: the file Calibre expects, or one with
// the format's extension.
QString formatFile(const QString &folder, const QString &name, const QString &format);

// list_trash_entries(): the books and formats in the library's trash.
QList<TrashEntry> trashEntries(const QString &libraryPath, const QString &interfaceLanguage);

} // namespace BookOperations
