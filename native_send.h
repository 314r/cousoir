#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>

#include <functional>

#include "python_json.h"

struct Book;
struct LibraryData;

// What a send without Calibre takes from Calibre's device drivers, whatever
// the reader: the format of a book it sends, the path the book goes to, its
// entry in metadata.calibre and the thumbnail of its cover.
namespace NativeSend {

// The format Calibre's send picks of `book`: the first of `formatMap`, the
// driver's ordered formats, the book has a file of; empty for none. Its
// file goes to `file`.
QString sendFormat(const Book &book, const QStringList &formatMap, QString *file);

// What a driver gives create_upload_path().
struct UploadOptions
{
    QString saveTemplate;
    // prefix_path: a Kobo's mount point, a Kindle's documents folder.
    QString prefix;
    // MAX_PATH_LEN.
    qsizetype maxLength = 250;
    bool useSubdirs = true;
    // filename_callback(): what goes before the extension, ".kepub".
    QString nameSuffix;
    // sanitize_path_components().
    std::function<void(QStringList *)> sanitizeComponents;
};

// create_upload_path(): where a book goes, relative to the prefix,
// `extension` its file's, ".azw3".
QString uploadPath(const LibraryData &data, const Book &book, const QString &extension, const UploadOptions &options,
                   const QString &interfaceLanguage);

// Calibre's ISO dates in metadata.calibre: "2015-07-07T18:41:43+00:00",
// "None" for the undefined date.
PythonJson::Value isoDate(const QDateTime &date);

// The thumbnail of the cover for metadata.calibre: [width, height, JPEG in
// base 64], within `width` by `height`, or `height` high for a width of 0;
// null without a cover.
PythonJson::Value thumbnail(const Book &book, int width, int height);
// The cover within `width` by `height`, and as a JPEG.
QImage thumbnailImage(const QImage &cover, int width, int height);
QByteArray thumbnailJpeg(const QImage &cover, int width, int height, int quality);

// The book's entry in metadata.calibre, as Calibre's JsonCodec writes it for
// a book it sent, and its text as an element of the file's array.
PythonJson::Value bookEntry(const LibraryData &data, const Book &book, const QString &lpath, qint64 size,
                            const PythonJson::Value &thumbnailValue);
QByteArray entryText(const PythonJson::Value &entry);

} // namespace NativeSend
