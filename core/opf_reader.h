#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

#include "calibre_date.h"
#include "python_json.h"

// The metadata Calibre reads from an ebook to add it, what its Metadata
// object holds: empty values are the ones the file did not have.
struct ImportedMetadata
{
    QString title;
    QString titleSort;
    QStringList authors;
    QString authorSort;
    QString series;
    std::optional<double> seriesIndex;
    QStringList tags;
    QString publisher;
    std::optional<CalibreDate> pubdate;
    // Calibre's own, in an OPF it wrote: kept when a book is restored.
    std::optional<CalibreDate> timestamp;
    QString uuid;
    QString comments;
    // ISO 639-2 codes.
    QStringList languages;
    QList<QPair<QString, QString>> identifiers;
    // 0 to 10.
    std::optional<double> rating;
    QString bookProducer;
    // An image's bytes.
    QByteArray cover;
    // Custom columns' values, as Calibre's user metadata holds them: each
    // column's field metadata by key ("#label"), with the value under
    // "#value#" and a series' number under "#extra#".
    QList<QPair<QString, PythonJson::Value>> userMetadata;

    void setIdentifier(const QString &type, const QString &value);
    // set_user_metadata(): false, and nothing set, for what is no field.
    bool setUserMetadata(const QString &key, PythonJson::Value field);
    const PythonJson::Value *userField(const QString &key) const;
    // smart_update()'s custom fields: the other's, multiple text merged
    // with this one's values.
    void updateUserMetadata(const ImportedMetadata &other);
};

// Reading OPF package files, ported from Calibre 9.16: OPF.to_book_metadata()
// in ebooks/metadata/opf2.py for version 2 and read_metadata() in
// ebooks/metadata/opf3.py for version 3.
namespace OpfReader {

// decode_is_multiple() of a field Calibre wrote: its separators from
// is_multiple2, else from what is_multiple says. False when it is no field.
bool decodeIsMultiple(PythonJson::Value *field);

// A field Calibre wrote as JSON, decoded: false when it is not one.
bool readUserField(const QString &json, PythonJson::Value *field);

struct Package
{
    ImportedMetadata metadata;
    // Relative to the OPF: the cover image, and the first item of the spine.
    QString rasterCover;
    QString epub3RasterCover;
    QString firstSpineItem;
    int version = 2;
};

// Empty `error` and an OPF's metadata, or false for something that is no
// OPF.
bool read(const QByteArray &opf, Package *package, QString *error);

// parse_date(): an ISO date or date and time; missing parts are those of the
// 15th of the current month, a missing time midnight. Without a timezone the
// date is UTC when `assumeUtc`, else local.
std::optional<CalibreDate> parseDate(const QString &text, bool assumeUtc);

// fix_only_date(): off the last day of a month, but for December's, and off
// the first.
QDateTime fixOnlyDate(const QDateTime &date);

} // namespace OpfReader
