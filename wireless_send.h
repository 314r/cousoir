#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "device_books.h"
#include "python_json.h"
#include "wireless.h"

struct Book;
struct LibraryData;

// What Calibre's GUI and its wireless driver make of the library's books for
// an app connected wirelessly: the format sent, the path it goes to, the
// metadata that goes with it, and, once the app's books are listed, the
// metadata of those the library changed since.
namespace WirelessSend {

// The format sent of `book`: the first the app takes that the book has.
QString format(const Book &book, const Wireless::App &app);

// _create_upload_path(): '{title} - {authors} ({id})' in ASCII, short
// enough for the app's paths; the uuid when the app asks for uuids.
QString uploadPath(const LibraryData &data, const Book &book, const QString &extension, const Wireless::App &app,
                   const QString &interfaceLanguage);

// The book's metadata as the driver sends it: its entry in
// metadata.calibre, with the type of its file, the thumbnail of its cover
// as the app wants it, and its series sorted as a title.
PythonJson::Value metadata(const LibraryData &data, const Book &book, const QString &lpath, qint64 size,
                           const Wireless::App &app, const QString &interfaceLanguage);

// The file sent of `book` in `format`, written to `target`: the library's
// metadata and cover written into an EPUB, an AZW3 or a MOBI copy as
// Calibre's send writes them (EpubMetadata, MobiMetadata), other formats
// copied as they are. False, with `error`, when it cannot be written.
bool prepareFile(const LibraryData &data, const Book &book, const QString &format, const QString &target,
                 const QString &interfaceLanguage, const QString &calibreConfigDirectory, QString *error);

// The app's books as Cousoir lists a reader's, matched with the library's.
QList<DeviceBook> deviceBooks(const QList<PythonJson::Value> &books, const LibraryData &data);

// set_books_in_library() with metadata managed on connect: the library's
// metadata of the books on the app whose last change differs from what the
// app has, or whose thumbnail is not the height it asks for.
QList<PythonJson::Value> changedMetadata(const QList<PythonJson::Value> &books, const LibraryData &data,
                                         const Wireless::App &app, const QString &interfaceLanguage);

} // namespace WirelessSend
