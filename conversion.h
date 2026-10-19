#pragma once

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

#include "book.h"

namespace CommitEngine {
struct Request;
}

// Converting a book of the library to another format as Calibre 9.16 does
// when you pick a format in "Convert books" and keep its settings: the book's
// settings saved by its last conversion over Calibre's defaults, the
// library's metadata and cover in the result, the result added to the book.
// Calibre does the work itself, through calibre-debug: Reliure hands it the
// book's files, its metadata as Calibre's create_opf_file() writes it and
// its cover, and takes back the converted file and the settings to keep.
//
// A job is a folder in the cache: job.json and what Calibre reads, then
// what it writes, log.txt and result.json.
namespace Conversion {

// The formats Reliure converts to, as Calibre names them.
QStringList outputFormats();

// supported_input_formats(): the book's formats Calibre converts from, in
// the order of its formats.
QStringList inputFormats(const Book &book);

// Calibre's calibre-debug, empty when Calibre is not installed.
QString program();

// Writes the job converting `book` of `data` to `format` into `folder`,
// which must exist and be empty. The book's metadata.opf supplies what
// Reliure does not keep, as for the metadata.opf Reliure writes.
bool prepare(const LibraryData &data, const Book &book, const QString &format, const QString &interfaceLanguage,
             const QString &folder, QString *error);

// What runs the job in `folder`.
QStringList arguments(const QString &folder);
// Calibre in `interfaceLanguage`, which names a table of contents and the
// like in the book, with its temporary files in the job's folder.
QProcessEnvironment environment(const QString &folder, const QString &interfaceLanguage);

// The share of the work done that a line of the job's output reports, from
// 0 to 1; -1 for a line that reports none.
double progress(const QByteArray &line);

struct Outcome
{
    bool ok = false;
    QString error;
    // The format converted from, in lower case as Calibre names it, which
    // can be ORIGINAL_EPUB's "epub" for a book that has one.
    QString inputFormat;
    // The converted file, in the job's folder.
    QString file;
    // Calibre keeps the file of the output format as ORIGINAL_<format>
    // before replacing it, when it converted a format to itself.
    bool saveOriginal = false;
    // The settings of the conversion, as Calibre keeps them for the book's
    // next one: the row of conversion_options.
    QByteArray options;
};

// What the job in `folder` came to, once Calibre is done; `exitCode` is
// calibre-debug's.
Outcome outcome(const QString &folder, int exitCode);

// Runs the job to its end and returns its outcome, telling `progress` how
// far it got: for the command line and the tests.
Outcome run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress = {});

// Adds the converted file to `request` as book_converted() of Calibre's
// convert action adds it: after ORIGINAL_<format>, when Calibre would keep
// it, and with the settings for the book's next conversion.
void addTo(CommitEngine::Request *request, const Book &book, const QString &format, const Outcome &outcome);

} // namespace Conversion
