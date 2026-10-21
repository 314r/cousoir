// What the golden tests do to or read from a library besides Calibre and
// Cousoir: the settings of earlier conversions, and the books of the real
// library they convert and send.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <functional>

#include "common.h"
#include "sqlite_db.h"

namespace Tool {

namespace {

// pickle_binary_string() of calibre/utils/__init__.py.
QByteArray pickled(const QByteArray &raw)
{
    QByteArray result("\x80\x02T", 3);
    const quint32 size = quint32(raw.size());
    for (int shift = 0; shift < 32; shift += 8)
        result.append(char((size >> shift) & 0xFF));
    return result + raw + '.';
}

// Each book's formats and their sizes.
using Formats = QMap<qint64, QMap<QString, qint64>>;

std::optional<Formats> formatsOf(SqliteDb &db)
{
    Formats formats;
    SqliteStatement data(db, QStringLiteral("SELECT book, format, uncompressed_size FROM data"));
    while (data.step())
        formats[data.integer(0)].insert(data.text(1), data.integer(2));
    if (!data.error().isEmpty()) {
        err() << data.error() << Qt::endl;
        return std::nullopt;
    }
    return formats;
}

// The books whose formats pass `test`, smallest `key` format first.
QList<qint64> smallest(const Formats &formats, const std::function<bool(const QMap<QString, qint64> &)> &test,
                       const QString &key)
{
    QList<std::pair<qint64, qint64>> found;
    for (auto it = formats.cbegin(); it != formats.cend(); ++it) {
        if (test(it.value()))
            found.append({ it.value().value(key), it.key() });
    }
    std::sort(found.begin(), found.end());
    QList<qint64> books;
    for (const auto &[size, book] : std::as_const(found))
        books.append(book);
    return books;
}

bool hasAny(const QMap<QString, qint64> &formats, std::initializer_list<const char *> names)
{
    return std::any_of(names.begin(), names.end(), [&formats](const char *name) {
        return formats.contains(QLatin1String(name));
    });
}

std::optional<Formats> openFormats(const QString &path, SqliteDb &db)
{
    QString error;
    if (!db.open(path, SqliteDb::Mode::Immutable, &error)) {
        err() << path << ": " << error << Qt::endl;
        return std::nullopt;
    }
    return formatsOf(db);
}

} // namespace

// Gives books of a copy of the fixture library the settings earlier
// conversions saved, as Calibre stores them in conversion_options:
//
// - book 1: a Python literal, as Calibre wrote them before JSON, which
//   Calibre 9.16 no longer reads;
// - book 2: AZW3 as the format to convert from, so that converting it to
//   AZW3 keeps its file as ORIGINAL_AZW3;
// - book 3: another output profile and smartened punctuation.
int conversionSettings(const QStringList &arguments)
{
    const std::pair<qint64, QByteArray> settings[] = {
        { 1, "{\n\t'gui_preferred_input_format' : 'azw3',\n\t'output_profile' : 'kindle_pw',\n"
             "\t'change_justification' : u'justify',\n}" },
        { 2, "json:{\n  \"gui_preferred_input_format\": \"azw3\"\n}" },
        { 3, "json:{\n  \"output_profile\": \"kobo\",\n  \"smarten_punctuation\": true,\n"
             "  \"gui_preferred_input_format\": \"epub\"\n}" },
    };
    SqliteDb db;
    QString error;
    if (!db.open(QDir(arguments.at(0)).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    for (const auto &[book, raw] : settings) {
        SqliteStatement insert(db,
                               QStringLiteral("INSERT INTO conversion_options(book, format, data) VALUES (?, 'PIPE', ?)"));
        insert.bind(1, book);
        insert.bind(2, pickled(raw));
        insert.step();
        if (!insert.error().isEmpty()) {
            err() << insert.error() << Qt::endl;
            return 1;
        }
    }
    return 0;
}

// Picks books of the real library to convert, with the user's Calibre
// settings: the two smallest EPUBs to KEPUB and AZW3, the smallest book with
// a KEPUB to KEPUB again, and the smallest whose only source is an AZW3 to
// AZW3, which keeps its file as ORIGINAL_AZW3; one Calibre converted before,
// so not locked by DRM. Writes them as cousoir-cli apply's changes and prints
// them as compare-libraries's --converted.
int pickConversions(const QStringList &arguments)
{
    SqliteDb db;
    const std::optional<Formats> formats = openFormats(arguments.at(0), db);
    if (!formats)
        return 1;
    QSet<qint64> converted;
    SqliteStatement options(db, QStringLiteral("SELECT book FROM conversion_options WHERE format='PIPE'"));
    while (options.step())
        converted.insert(options.integer(0));
    const QList<qint64> epubs = smallest(*formats, [](const auto &f) { return hasAny(f, { "EPUB" }); }, QStringLiteral("EPUB"));
    const QList<qint64> kepubs = smallest(
        *formats, [](const auto &f) { return f.contains(QLatin1String("EPUB")) && f.contains(QLatin1String("KEPUB")); },
        QStringLiteral("EPUB"));
    QList<qint64> azw3s;
    for (const qint64 book : smallest(
             *formats,
             [](const auto &f) {
                 return f.contains(QLatin1String("AZW3")) && !hasAny(f, { "EPUB", "MOBI", "LIT", "PRC", "FB2", "DOCX" });
             },
             QStringLiteral("AZW3"))) {
        if (converted.contains(book))
            azw3s.append(book);
    }
    QList<std::pair<qint64, QString>> picks;
    const std::pair<QList<qint64>, QString> choices[] = {
        { epubs.mid(0, 1), QStringLiteral("KEPUB") },
        { epubs.mid(1, 1), QStringLiteral("AZW3") },
        { kepubs.mid(0, 1), QStringLiteral("KEPUB") },
        { azw3s.mid(0, 1), QStringLiteral("AZW3") },
    };
    for (const auto &[books, format] : choices) {
        for (const qint64 book : books) {
            if (!picks.contains(std::pair{ book, format }))
                picks.append({ book, format });
        }
    }
    QJsonArray changes;
    QStringList listed;
    for (const auto &[book, format] : std::as_const(picks)) {
        changes.append(QJsonObject{
            { QStringLiteral("convert"),
              QJsonObject{ { QStringLiteral("book"), book }, { QStringLiteral("format"), format } } },
        });
        listed.append(QStringLiteral("%1:%2").arg(book).arg(format));
    }
    if (!writeFile(arguments.at(1), QJsonDocument(changes).toJson(QJsonDocument::Compact)))
        return 1;
    out() << listed.join(u',') << '\n';
    return 0;
}

// Picks books of the real library to send: for a Kobo, the smallest EPUB
// without a KEPUB, made a KEPUB on the way, the smallest with a KEPUB and the
// smallest PDF; for a Kindle, the smallest AZW3, the smallest MOBI without
// one and the smallest PDF. Prints them, then the first, to send again, then
// the second, to remove: "[1, 2, 3] [1] 2".
int pickSends(const QStringList &arguments)
{
    SqliteDb db;
    const std::optional<Formats> formats = openFormats(arguments.at(0), db);
    if (!formats)
        return 1;
    const auto first = [&formats](const std::function<bool(const QMap<QString, qint64> &)> &test, const char *key) {
        const QList<qint64> books = smallest(*formats, test, QLatin1String(key));
        return books.isEmpty() ? std::nullopt : std::optional<qint64>(books.first());
    };
    QList<std::optional<qint64>> found;
    if (arguments.at(1) == QLatin1String("kobo")) {
        found = {
            first([](const auto &f) { return f.contains(QLatin1String("EPUB")) && !f.contains(QLatin1String("KEPUB")); },
                  "EPUB"),
            first([](const auto &f) { return f.contains(QLatin1String("KEPUB")); }, "KEPUB"),
            first([](const auto &f) { return f.contains(QLatin1String("PDF")) && !hasAny(f, { "KEPUB", "EPUB" }); },
                  "PDF"),
        };
    } else {
        found = {
            first([](const auto &f) { return f.contains(QLatin1String("AZW3")); }, "AZW3"),
            first([](const auto &f) { return f.contains(QLatin1String("MOBI")) && !f.contains(QLatin1String("AZW3")); },
                  "MOBI"),
            first([](const auto &f) { return f.contains(QLatin1String("PDF")) && !hasAny(f, { "AZW3", "MOBI", "AZW" }); },
                  "PDF"),
        };
    }
    QStringList picks;
    for (const std::optional<qint64> &book : std::as_const(found)) {
        if (book && !picks.contains(QString::number(*book)))
            picks.append(QString::number(*book));
    }
    if (picks.isEmpty()) {
        err() << "No book of the library to send" << Qt::endl;
        return 1;
    }
    out() << '[' << picks.join(QStringLiteral(", ")) << "] [" << picks.first() << "] " << picks.value(1, picks.first())
          << '\n';
    return 0;
}

// The text of a key of a JSON file's object, empty when it has none.
int jsonGet(const QStringList &arguments)
{
    const std::optional<QJsonValue> read = readJson(arguments.at(0));
    if (!read)
        return 1;
    out() << read->toObject().value(arguments.at(1)).toString() << '\n';
    return 0;
}

} // namespace Tool
