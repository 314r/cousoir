#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>

// One file of a book, from Calibre's data table.
struct BookFormat
{
    // Upper case, like "EPUB".
    QString format;
    // The file name without its extension.
    QString name;
    qint64 size = 0;
    // The file found in the book's folder; empty when it is missing.
    QString file;
};

// A custom column of the library, as custom_columns describes it.
struct CustomColumn
{
    int id = 0;
    // "genre" for #genre.
    QString label;
    QString name;
    // text, comments, series, enumeration, int, float, rating, datetime,
    // bool or composite.
    QString datatype;
    // Several values per book, as tags have.
    bool multiple = false;
    // Its values are items with a link table, as authors and tags are.
    bool normalized = false;
    bool editable = true;
    // How Calibre shows it, as JSON: the template of a composite, the values
    // of an enumeration, half stars.
    QByteArray display;
};

// A book's value of a custom column: null when it has none. A list for a
// column of several values; else text, a number, a date or true and false.
struct CustomValue
{
    QVariant value;
    // The book's number in a series column.
    double seriesIndex = 1;
};

// A row of Calibre's books table and the items linked to it. Ids are those
// of metadata.db; 0 means no item.
struct Book
{
    int id = 0;
    QString title;
    QString sort;
    QString authorSort;
    // In link order; a "|" in the database is a comma in the name.
    QList<int> authorIds;
    QStringList authors;
    int seriesId = 0;
    QString series;
    double seriesIndex = 1;
    QList<int> tagIds;
    QStringList tags;
    int publisherId = 0;
    QString publisher;
    // 0 for none, else 1 to 10: two per star.
    int rating = 0;
    QList<int> languageIds;
    // ISO 639-2 codes.
    QStringList languages;
    // Type and value, in database order.
    QList<QPair<QString, QString>> identifiers;
    // UTC, to the second. Calibre's "undefined" date is 0101-01-01.
    QDateTime timestamp;
    QDateTime pubdate;
    QDateTime lastModified;
    // The book's folder, relative to the library.
    QString path;
    QString uuid;
    bool hasCover = false;
    // Its page count, as Calibre counted it; 0 until it does, -1 without a
    // row in books_pages_link.
    int pages = -1;
    // HTML.
    QString comments;
    // Sorted by format, like Calibre.
    QList<BookFormat> formats;
    // The settings Calibre's last conversion of the book saved for the next
    // one, its conversion_options row as stored; empty when there is none.
    QByteArray conversionOptions;

    // Its custom columns' values, in the order of the library's columns.
    QList<CustomValue> custom;

    // From the book's folder rather than the database: its cover.jpg, empty
    // when there is none, and the cover's modification time in ms.
    QString coverFile;
    qint64 coverModified = 0;
};

// A row of a browse section: an author, series, tag, publisher or language,
// with the number of books linked to it.
struct FacetItem
{
    int id = 0;
    QString name;
    QString sort;
    int count = 0;
    // The item's link column: a web page about it, empty for most.
    QString link;
};

// Enough of a file to notice that it changed: size, modification time and,
// where the platform has one, its inode.
struct FileIdentity
{
    bool exists = false;
    qint64 size = -1;
    qint64 modifiedMs = 0;
    quint64 inode = 0;

    bool operator==(const FileIdentity &) const = default;
};

struct LibraryData
{
    QString libraryPath;
    // library_id: Calibre's id for the library.
    QString uuid;
    int userVersion = 0;
    QString schemaFingerprint;
    bool schemaKnown = false;
    QList<Book> books;
    QList<FacetItem> authors;
    QList<FacetItem> series;
    QList<FacetItem> tags;
    QList<FacetItem> publishers;
    QList<FacetItem> languages;
    // The age at which Calibre deletes trash entries: the library's
    // expire_old_trash_after, 14 days unless changed, never under a day.
    qint64 trashExpirySeconds = 14 * 24 * 3600;
    // metadata.db as it was copied.
    FileIdentity source;
    // The custom columns, by id.
    QList<CustomColumn> customColumns;
    // The preferences a search reads, by key, as JSON: saved searches,
    // virtual libraries, grouped search terms and user categories; and the
    // columns and sort Calibre's list last had.
    QHash<QString, QString> preferences;

    // The book with that id, or nullptr.
    const Book *book(int id) const
    {
        for (const Book &candidate : books) {
            if (candidate.id == id)
                return &candidate;
        }
        return nullptr;
    }
};
