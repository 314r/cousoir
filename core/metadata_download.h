#pragma once

#include <QDate>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

struct Book;
struct CalibreConfig;
struct LibraryData;

// Downloading a book's metadata and covers from the web, as Calibre's Edit
// metadata dialog does: Calibre's own sources do the work, run by
// calibre-debug in a job folder of the cache (see CalibreJob), and Cousoir
// reads what they found.
namespace MetadataDownload {

// What a book is looked for by: its title, authors and identifiers.
struct Query
{
    QString title;
    QStringList authors;
    QList<QPair<QString, QString>> identifiers;
    // Seconds each source has.
    int timeout = 30;
};

// A book the sources found, as identify() merges and orders the results.
struct Found
{
    QString title;
    QStringList authors;
    QString authorSort;
    QString publisher;
    // The day it was published, without time or zone.
    std::optional<QDate> pubdate;
    QString series;
    std::optional<double> seriesIndex;
    QStringList tags;
    // 0 to 5 stars.
    std::optional<double> rating;
    QString comments;
    // ISO 639-2 codes.
    QStringList languages;
    QList<QPair<QString, QString>> identifiers;
    bool hasCover = false;
};

struct Identified
{
    QList<Found> books;
    // Calibre's preferences for downloads: the fields left alone, and
    // whether a summary is added to the book's own.
    QStringList ignoredFields;
    bool appendComments = false;
};

struct Cover
{
    QString source;
    int width = 0;
    int height = 0;
    // The image, in the job's folder.
    QString file;
};

// calibre-debug, empty when Calibre is not installed.
QString program();
// Writes the job of looking for the book, or of downloading covers for
// it, in `folder`: its Python and its query. The covers job runs in the
// folder of the identify job before it, whose sources' caches find the
// covers again.
bool prepareIdentify(const QString &folder, const Query &query, QString *error);
bool prepareCovers(const QString &folder, const Query &query, QString *error);
// The arguments calibre-debug runs a prepared job with.
QStringList arguments(const QString &folder);

// What a job found, or false with `error`, Calibre's log saying why.
bool readIdentified(const QString &folder, Identified *identified, QString *error);
bool readCovers(const QString &folder, QList<Cover> *covers, QString *error);
// The covers a covers job has saved so far, in the order they came.
QList<Cover> coversFound(const QString &folder);

// The fields a download changes, as changes() names them.
const QStringList &fields();
// update_from_mi() of Calibre's Edit metadata dialog: what `found` changes
// of the book, for the fields chosen, as BookEdit takes it. The title sort
// and author sort are generated again; the found tags come before the
// book's, its identifiers are updated, and the found summary is added to
// its own with `appendComments`.
QVariantMap changes(const LibraryData &data, const Book &book, const CalibreConfig &config, const Found &found,
                    const QStringList &fields, bool appendComments);

} // namespace MetadataDownload
