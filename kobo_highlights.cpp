// SPDX-License-Identifier: GPL-3.0-only
//
// The chapters are found as kbextract2 finds them,
// https://github.com/314r/kbextract2, Copyright (c) 2026 314r, MIT license:
// internal/kobo/reader.go, readKepubChapters() and
// loadResolvedAnnotations(); internal/kobo/paths.go, readSerialNumber().

#include "kobo_highlights.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>

#include "cousoir_paths.h"
#include "device_books.h"
#include "sqlite_db.h"

namespace KoboHighlights {

namespace {

QSet<QString> columnsOf(SqliteDb &db, const QString &table)
{
    QSet<QString> columns;
    SqliteStatement info(db, QStringLiteral("PRAGMA table_info(%1)").arg(table));
    while (info.step())
        columns.insert(info.text(1).toLower());
    return columns;
}

struct Chapter
{
    QString key;
    QString title;
    std::optional<qint64> order;
    std::optional<qint64> spineStart;
};

// readKepubChapters(): a KEPUB's table of contents, its rows of type 899,
// by their ids, the ids without "-N" and the files they open on; and those
// whose file has a place in the book, by that place.
void readChapters(SqliteDb &db, const QString &volumeId, bool bookmarked, QHash<QString, Chapter> *chapters,
                  QList<Chapter> *ranges)
{
    SqliteStatement rows(db, QStringLiteral("SELECT navigation.ContentID, %1, navigation.Title, navigation.VolumeIndex, "
                                            "target.VolumeIndex FROM content navigation "
                                            "LEFT JOIN content target ON target.ContentID = %1 "
                                            "WHERE CAST(navigation.ContentType AS INTEGER) = 899 AND navigation.BookID = ? "
                                            "ORDER BY CASE WHEN target.VolumeIndex IS NULL THEN 1 ELSE 0 END, "
                                            "target.VolumeIndex, COALESCE(navigation.VolumeIndex, 0), "
                                            "navigation.ContentID COLLATE NOCASE")
                                 .arg(bookmarked ? QStringLiteral("navigation.ChapterIDBookmarked") : QStringLiteral("NULL")));
    rows.bind(1, volumeId);
    while (rows.step()) {
        const QString contentId = HighlightFormats::normalizedText(rows.text(0));
        if (contentId.isEmpty())
            continue;
        const QString target = HighlightFormats::normalizedText(rows.text(1));
        Chapter chapter;
        chapter.key = target.isEmpty() ? chapterIdOf(contentId) : target;
        chapter.title = HighlightFormats::singleLineText(rows.text(2));
        if (!rows.isNull(4))
            chapter.spineStart = rows.integer(4);
        chapter.order = chapter.spineStart ? chapter.spineStart
                                           : (rows.isNull(3) ? std::nullopt : std::optional<qint64>(rows.integer(3)));
        // Later rows over earlier ones for the same file.
        chapters->insert(contentId, chapter);
        chapters->insert(chapterIdOf(contentId), chapter);
        if (!chapter.key.isEmpty())
            chapters->insert(chapter.key, chapter);
        if (chapter.spineStart)
            ranges->append(chapter);
    }
    std::stable_sort(ranges->begin(), ranges->end(),
                     [](const Chapter &a, const Chapter &b) { return *a.spineStart < *b.spineStart; });
}

QString formatOfVolume(const QString &volumeId)
{
    const QString lower = volumeId.toLower();
    if (lower.endsWith(QLatin1String(".kepub.epub")))
        return QStringLiteral("KEPUB");
    const QString suffix = QFileInfo(lower.section(u'/', -1)).suffix();
    // A book bought from Kobo goes by its id alone.
    return suffix.isEmpty() ? QStringLiteral("KEPUB") : suffix.toUpper();
}

QJsonValue optionalNumber(const std::optional<qint64> &value)
{
    return value ? QJsonValue(double(*value)) : QJsonValue();
}

std::optional<qint64> numberOf(const QJsonValue &value)
{
    return value.isDouble() ? std::optional<qint64>(qint64(value.toDouble())) : std::nullopt;
}

QString snapshotFile(const QString &device)
{
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(device.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    return QDir(CousoirPaths::koboFolder()).filePath(key + QStringLiteral(".json"));
}

QJsonArray bookArray(const QList<Book> &books)
{
    QJsonArray array;
    for (const Book &book : books) {
        QJsonArray highlights;
        for (const Highlight &highlight : book.highlights) {
            highlights.append(QJsonObject{
                { QStringLiteral("id"), highlight.id },
                { QStringLiteral("content_id"), highlight.contentId },
                { QStringLiteral("start_path"), highlight.startPath },
                { QStringLiteral("start_offset"), double(highlight.startOffset) },
                { QStringLiteral("end_path"), highlight.endPath },
                { QStringLiteral("end_offset"), double(highlight.endOffset) },
                { QStringLiteral("text"), highlight.text },
                { QStringLiteral("note"), highlight.note },
                { QStringLiteral("created"), highlight.created },
                { QStringLiteral("modified"), highlight.modified },
                { QStringLiteral("color"), highlight.color },
                { QStringLiteral("hidden"), highlight.hidden },
                { QStringLiteral("chapter_progress"), highlight.chapterProgress },
                { QStringLiteral("chapter_key"), highlight.chapterKey },
                { QStringLiteral("chapter_title"), highlight.chapterTitle },
                { QStringLiteral("chapter_order"), optionalNumber(highlight.chapterOrder) },
            });
        }
        array.append(QJsonObject{
            { QStringLiteral("volume_id"), book.volumeId },
            { QStringLiteral("title"), book.title },
            { QStringLiteral("authors"), QJsonArray::fromStringList(book.authors) },
            { QStringLiteral("lpath"), book.lpath },
            { QStringLiteral("uuid"), book.uuid },
            { QStringLiteral("application_id"), book.applicationId },
            { QStringLiteral("format"), book.format },
            { QStringLiteral("purchased"), book.purchased },
            { QStringLiteral("on_device"), book.onDevice },
            { QStringLiteral("highlights"), highlights },
        });
    }
    return array;
}

QList<Book> booksFrom(const QJsonArray &array)
{
    QList<Book> books;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        Book book;
        book.volumeId = object.value(QLatin1String("volume_id")).toString();
        book.title = object.value(QLatin1String("title")).toString();
        for (const QJsonValue &author : object.value(QLatin1String("authors")).toArray())
            book.authors.append(author.toString());
        book.lpath = object.value(QLatin1String("lpath")).toString();
        book.uuid = object.value(QLatin1String("uuid")).toString();
        book.applicationId = object.value(QLatin1String("application_id")).toInt();
        book.format = object.value(QLatin1String("format")).toString();
        book.purchased = object.value(QLatin1String("purchased")).toBool();
        book.onDevice = object.value(QLatin1String("on_device")).toBool();
        for (const QJsonValue &item : object.value(QLatin1String("highlights")).toArray()) {
            const QJsonObject h = item.toObject();
            Highlight highlight;
            highlight.id = h.value(QLatin1String("id")).toString();
            highlight.volumeId = book.volumeId;
            highlight.contentId = h.value(QLatin1String("content_id")).toString();
            highlight.startPath = h.value(QLatin1String("start_path")).toString();
            highlight.startOffset = qint64(h.value(QLatin1String("start_offset")).toDouble());
            highlight.endPath = h.value(QLatin1String("end_path")).toString();
            highlight.endOffset = qint64(h.value(QLatin1String("end_offset")).toDouble());
            highlight.text = h.value(QLatin1String("text")).toString();
            highlight.note = h.value(QLatin1String("note")).toString();
            highlight.created = h.value(QLatin1String("created")).toString();
            highlight.modified = h.value(QLatin1String("modified")).toString();
            highlight.color = h.value(QLatin1String("color")).toInt(-1);
            highlight.hidden = h.value(QLatin1String("hidden")).toBool();
            highlight.chapterProgress = h.value(QLatin1String("chapter_progress")).toDouble();
            highlight.chapterKey = h.value(QLatin1String("chapter_key")).toString();
            highlight.chapterTitle = h.value(QLatin1String("chapter_title")).toString();
            highlight.chapterOrder = numberOf(h.value(QLatin1String("chapter_order")));
            book.highlights.append(highlight);
        }
        books.append(book);
    }
    return books;
}

} // namespace

QString Snapshot::digest() const
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(bookArray(books)).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha1)
                                   .toHex());
}

QString chapterIdOf(const QString &contentId)
{
    const qsizetype dash = contentId.lastIndexOf(u'-');
    return dash > contentId.lastIndexOf(u'!') ? contentId.left(dash) : contentId;
}

QList<Book> read(SqliteDb &db, QString *error)
{
    const QSet<QString> bookmark = columnsOf(db, QStringLiteral("Bookmark"));
    const QSet<QString> content = columnsOf(db, QStringLiteral("content"));
    for (const char *name : { "bookmarkid", "volumeid", "contentid", "text", "annotation" }) {
        if (!bookmark.contains(QLatin1String(name))) {
            if (error)
                *error = QObject::tr("This is no Kobo database: it has no highlights.");
            return {};
        }
    }
    if (!content.contains(QStringLiteral("contentid")) || !content.contains(QStringLiteral("title"))) {
        if (error)
            *error = QObject::tr("This is no Kobo database: it has no books.");
        return {};
    }
    const auto column = [&bookmark](const char *name) {
        return bookmark.contains(QString::fromLatin1(name).toLower()) ? QStringLiteral("bm.") + QLatin1String(name)
                                                                        : QStringLiteral("NULL");
    };
    const QString volumeIndex = content.contains(QStringLiteral("volumeindex")) ? QStringLiteral("chapter.VolumeIndex")
                                                                                 : QStringLiteral("NULL");
    const QStringList columns{ QStringLiteral("bm.BookmarkID"), QStringLiteral("bm.VolumeID"), QStringLiteral("bm.ContentID"),
                               QStringLiteral("bm.Text"), QStringLiteral("bm.Annotation"), column("Hidden"),
                               column("StartContainerPath"), column("StartContainerChildIndex"), column("StartOffset"),
                               column("EndContainerPath"), column("EndOffset"), column("DateCreated"),
                               column("DateModified"), column("Color"), column("Type"), column("ChapterProgress"),
                               QStringLiteral("chapter.Title"), volumeIndex };
    SqliteStatement rows(db, QStringLiteral("SELECT %1 FROM Bookmark bm LEFT JOIN content chapter ON chapter.ContentID = bm.ContentID "
                                            "WHERE bm.VolumeID IS NOT NULL AND TRIM(CAST(bm.VolumeID AS TEXT)) <> '' "
                                            "ORDER BY bm.VolumeID, CASE WHEN %2 IS NULL THEN 1 ELSE 0 END, %2, COALESCE(%3, 0), "
                                            "bm.ContentID COLLATE NOCASE, bm.BookmarkID COLLATE NOCASE")
                                 .arg(columns.join(QStringLiteral(", ")), volumeIndex, column("ChapterProgress")));
    if (!rows.isValid()) {
        if (error)
            *error = QObject::tr("Cannot read the Kobo's highlights: %1").arg(rows.error());
        return {};
    }
    const bool bookmarked = content.contains(QStringLiteral("chapteridbookmarked"));
    const bool typed = content.contains(QStringLiteral("contenttype")) && content.contains(QStringLiteral("bookid"));
    QList<Book> books;
    QHash<QString, Chapter> chapters;
    QList<Chapter> ranges;
    while (rows.step()) {
        const QString volumeId = HighlightFormats::normalizedText(rows.text(1));
        if (books.isEmpty() || books.last().volumeId != volumeId) {
            Book book;
            book.volumeId = volumeId;
            book.format = formatOfVolume(volumeId);
            books.append(book);
            chapters.clear();
            ranges.clear();
            if (typed)
                readChapters(db, volumeId, bookmarked, &chapters, &ranges);
        }
        Highlight highlight;
        highlight.id = HighlightFormats::normalizedText(rows.text(0));
        highlight.volumeId = volumeId;
        highlight.contentId = HighlightFormats::normalizedText(rows.text(2));
        highlight.text = HighlightFormats::normalizedText(rows.text(3));
        highlight.note = HighlightFormats::normalizedText(rows.text(4));
        const QString hidden = rows.text(5).trimmed().toLower();
        highlight.hidden = !(rows.isNull(5) || hidden == QLatin1String("false") || hidden == QLatin1String("0"));
        highlight.startPath = rows.text(6);
        highlight.startOffset = rows.integer(8);
        highlight.endPath = rows.text(9);
        highlight.endOffset = rows.integer(10);
        highlight.created = rows.text(11);
        highlight.modified = rows.text(12);
        highlight.color = rows.isNull(13) ? -1 : int(rows.integer(13));
        highlight.chapterProgress = rows.real(15);
        // A corner turned down, or a drawing, has nothing to keep; without
        // a type, a mark at the very start of a file is a corner, as
        // Calibre and kbextract2 take it.
        const QString type = rows.text(14).trimmed().toLower();
        const bool corner = type.isEmpty() ? rows.integer(7) == 0 && highlight.startOffset == 0
                                           : type == QLatin1String("dogear") || type == QLatin1String("markup");
        if (corner || (highlight.text.isEmpty() && highlight.note.isEmpty()))
            continue;
        // loadResolvedAnnotations(): the chapter of the file, else of the
        // table of contents' entry it comes after.
        highlight.chapterKey = highlight.contentId;
        highlight.chapterTitle = HighlightFormats::singleLineText(rows.text(16));
        const std::optional<qint64> place = rows.isNull(17) ? std::nullopt : std::optional<qint64>(rows.integer(17));
        highlight.chapterOrder = place;
        std::optional<Chapter> resolved;
        if (chapters.contains(highlight.contentId)) {
            resolved = chapters.value(highlight.contentId);
        } else if (place) {
            for (const Chapter &chapter : std::as_const(ranges)) {
                if (*chapter.spineStart > *place)
                    break;
                resolved = chapter;
            }
        }
        if (resolved) {
            highlight.chapterKey = resolved->key;
            if (!resolved->title.isEmpty())
                highlight.chapterTitle = resolved->title;
            if (resolved->order)
                highlight.chapterOrder = resolved->order;
        }
        books.last().highlights.append(highlight);
    }
    // Each book's title and authors, the book's own row of the database.
    for (Book &book : books) {
        const QList<QVariantList> found = db.rows(
            QStringLiteral("SELECT Title, %1 FROM content WHERE ContentID = ?%2")
                .arg(content.contains(QStringLiteral("attribution")) ? QStringLiteral("Attribution") : QStringLiteral("NULL"),
                     content.contains(QStringLiteral("bookid")) ? QStringLiteral(" AND BookID IS NULL") : QString()),
            { book.volumeId });
        for (const QVariantList &row : found) {
            if (book.title.isEmpty())
                book.title = HighlightFormats::singleLineText(row.value(0).toString());
            if (book.authors.isEmpty()) {
                for (const QString &author : row.value(1).toString().split(u'&', Qt::SkipEmptyParts))
                    book.authors.append(author.trimmed());
            }
        }
        if (book.title.isEmpty())
            book.title = book.volumeId;
    }
    books.removeIf([](const Book &book) { return book.highlights.isEmpty(); });
    std::stable_sort(books.begin(), books.end(), [](const Book &a, const Book &b) {
        const int byTitle = a.title.compare(b.title, Qt::CaseInsensitive);
        return byTitle != 0 ? byTitle < 0 : a.authors.join(u'&').compare(b.authors.join(u'&'), Qt::CaseInsensitive) < 0;
    });
    return books;
}

void attach(QList<Book> *books, const QList<DeviceBook> &deviceBooks)
{
    QHash<QString, const DeviceBook *> byContentId;
    for (const DeviceBook &deviceBook : deviceBooks) {
        if (!deviceBook.contentId.isEmpty())
            byContentId.insert(deviceBook.contentId, &deviceBook);
    }
    for (Book &book : *books) {
        const DeviceBook *deviceBook = byContentId.value(book.volumeId);
        book.onDevice = deviceBook != nullptr;
        if (!deviceBook)
            continue;
        book.lpath = deviceBook->lpath;
        book.uuid = deviceBook->uuid;
        book.applicationId = deviceBook->applicationId;
        book.purchased = deviceBook->purchased;
        if (!deviceBook->format.isEmpty())
            book.format = deviceBook->format;
    }
}

QList<int> match(const QList<Book> &books, const LibraryData &data)
{
    QList<DeviceBook> standIns;
    for (const Book &book : books) {
        DeviceBook standIn;
        standIn.title = book.title;
        standIn.authors = book.authors;
        standIn.uuid = book.uuid;
        standIn.applicationId = book.applicationId;
        standIns.append(standIn);
    }
    DeviceBooks::match(&standIns, data);
    QList<int> ids;
    for (const DeviceBook &standIn : std::as_const(standIns))
        ids.append(standIn.bookId);
    return ids;
}

HighlightFormats::Entry entry(const Highlight &highlight)
{
    return { highlight.id, highlight.text, highlight.note, highlight.chapterKey, highlight.chapterTitle,
             highlight.chapterOrder };
}

QString calibreColor(int color)
{
    switch (color) {
    case 1:
        return QStringLiteral("red");
    case 2:
        return QStringLiteral("blue");
    case 3:
        return QStringLiteral("green");
    default:
        return QStringLiteral("yellow");
    }
}

QString readSerialNumber(const QString &confPath)
{
    QFile file(confPath);
    if (confPath.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(u'#') || line.startsWith(u';') || line.startsWith(u'['))
            continue;
        const qsizetype equals = line.indexOf(u'=');
        if (equals < 0 || line.left(equals).trimmed() != QLatin1String("SerialNumber"))
            continue;
        const QString serial = line.mid(equals + 1).trimmed();
        if (!serial.isEmpty())
            return serial;
    }
    return {};
}

Identity identify(const QString &databasePath)
{
    const QFileInfo info(databasePath);
    QDir folder = info.absoluteDir();
    Identity identity;
    if (folder.dirName() == QLatin1String(".kobo")) {
        QFile version(folder.filePath(QStringLiteral("version")));
        if (version.open(QIODevice::ReadOnly))
            identity.serial = QString::fromUtf8(version.readAll()).trimmed().section(u',', 0, 0).trimmed();
        if (identity.serial.isEmpty())
            identity.serial = readSerialNumber(folder.filePath(QStringLiteral("Kobo/Kobo eReader.conf")));
        folder.cdUp();
    }
    identity.name = folder.dirName();
    if (identity.name.isEmpty())
        identity.name = info.completeBaseName();
    return identity;
}

QJsonArray toJson(const QList<Book> &books)
{
    return bookArray(books);
}

QByteArray toJson(const Snapshot &snapshot)
{
    QJsonObject synced;
    for (auto it = snapshot.synced.cbegin(); it != snapshot.synced.cend(); ++it)
        synced.insert(it.key(), it.value());
    return QJsonDocument(QJsonObject{
                             { QStringLiteral("device"), snapshot.device },
                             { QStringLiteral("name"), snapshot.name },
                             { QStringLiteral("source"), snapshot.source },
                             { QStringLiteral("seen"), snapshot.seen.toUTC().toString(Qt::ISODate) },
                             { QStringLiteral("synced"), synced },
                             { QStringLiteral("books"), bookArray(snapshot.books) },
                         })
        .toJson(QJsonDocument::Compact);
}

std::optional<Snapshot> fromJson(const QByteArray &json)
{
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (object.value(QLatin1String("device")).toString().isEmpty())
        return std::nullopt;
    Snapshot snapshot;
    snapshot.device = object.value(QLatin1String("device")).toString();
    snapshot.name = object.value(QLatin1String("name")).toString();
    snapshot.source = object.value(QLatin1String("source")).toString();
    snapshot.seen = QDateTime::fromString(object.value(QLatin1String("seen")).toString(), Qt::ISODate);
    const QJsonObject synced = object.value(QLatin1String("synced")).toObject();
    for (auto it = synced.constBegin(); it != synced.constEnd(); ++it)
        snapshot.synced.insert(it.key(), it.value().toString());
    snapshot.books = booksFrom(object.value(QLatin1String("books")).toArray());
    return snapshot;
}

std::optional<Snapshot> readSnapshot(const QString &device)
{
    QFile file(snapshotFile(device));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return fromJson(file.readAll());
}

bool writeSnapshot(const Snapshot &snapshot, QString *error)
{
    const QString path = snapshotFile(snapshot.device);
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(toJson(snapshot)) < 0 || !file.commit()) {
        if (error)
            *error = QObject::tr("Cannot keep the Kobo's highlights in %1.").arg(path);
        return false;
    }
    return true;
}

QList<Snapshot> snapshots()
{
    QList<Snapshot> found;
    const QDir folder(CousoirPaths::koboFolder());
    for (const QString &name : folder.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name)) {
        QFile file(folder.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (std::optional<Snapshot> snapshot = fromJson(file.readAll()))
            found.append(*snapshot);
    }
    return found;
}

} // namespace KoboHighlights
