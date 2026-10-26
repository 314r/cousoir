// SPDX-License-Identifier: GPL-3.0-only
//
// Writes to a Kobo's database as Calibre 9.16 guards and makes its own:
// - src/calibre/devices/kobo/driver.py: KOBOTOUCH.supported_dbversion,
//   max_supported_fwversion and modify_database_check(); set_readstatus()
// - src/calibre/devices/kobo/db.py: Database.copy_db(), the checkpoint
// The rows themselves are as the Kobo writes its own.

#include "kobo_write.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cmath>
#include <memory>
#include <optional>

#include "cousoir_paths.h"
#include "highlight_formats.h"
#include "kobo.h"
#include "kobo_highlights.h"
#include "kobo_placement.h"
#include "kobo_sync.h"
#include "sqlite_db.h"

namespace KoboWrite {

namespace {

// KOBOTOUCH's: the newest database and firmware Calibre writes to.
const int supportedDbVersion = 222;
const QList<int> maxFirmware{ 6, 0, 274403 };
// The copies of a Kobo's database kept.
const int backups = 5;

const QString onboard = QStringLiteral("file:///mnt/onboard/");

QString databaseOf(const QString &root)
{
    return QDir(root).filePath(QStringLiteral(".kobo/KoboReader.sqlite"));
}

// What tells that the database changed: the size and time of it and of
// the log beside it.
QList<qint64> identityOf(const QString &root)
{
    QList<qint64> identity;
    for (const QString &path : { databaseOf(root), databaseOf(root) + QStringLiteral("-wal") }) {
        const QFileInfo info(path);
        identity << (info.exists() ? info.size() : -1) << (info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0);
    }
    return identity;
}

QList<int> firmwareOf(const QString &root)
{
    QFile file(QDir(root).filePath(QStringLiteral(".kobo/version")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<int> version;
    for (const QString &part : QString::fromUtf8(file.readAll()).trimmed().section(u',', 2, 2).split(u'.'))
        version.append(part.toInt());
    return version;
}

double secondsOf(const QDateTime &time)
{
    return time.isValid() ? double(time.toMSecsSinceEpoch()) / 1000 : 0;
}

QDateTime timeOf(double seconds)
{
    return QDateTime::fromMSecsSinceEpoch(qint64(seconds * 1000), QTimeZone::UTC);
}

// "epubcfi(/6/4/2/1:0)": the file's index in the spine, and the place in it.
std::optional<QPair<int, QString>> splitCfi(const QString &cfi)
{
    static const QRegularExpression full(QStringLiteral("^epubcfi\\(/(\\d+)(.*)\\)$"));
    const QRegularExpressionMatch match = full.match(cfi);
    if (!match.hasMatch())
        return std::nullopt;
    const int step = match.captured(1).toInt();
    if (step < 2 || step % 2)
        return std::nullopt;
    return QPair<int, QString>(step / 2 - 1, match.captured(2));
}

// What the Kobo has of a book, and of a highlight.
struct KoboBook
{
    int status = 0;
    QDateTime lastRead;
};

struct KoboMark
{
    QString volumeId;
    QDateTime modified;
    QString note;
    int color = -1;
};

QString quoted(const QString &text)
{
    const QString line = HighlightFormats::singleLineText(text);
    return QStringLiteral("“%1”").arg(line.size() > 60 ? line.left(59) + u'…' : line);
}

} // namespace

QString backupFolder(const QString &device)
{
    const QString key = QString::fromLatin1(QCryptographicHash::hash(device.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    return QDir(CousoirPaths::koboFolder()).filePath(QStringLiteral("backups/") + key);
}

Changes plan(const Input &input, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return Changes();
    };
    const QString database = databaseOf(input.root);
    if (!Kobo::isKobo(input.root) || !QFileInfo(database).isFile())
        return fail(QObject::tr("The Kobo is no longer there."));
    const QList<int> firmware = firmwareOf(input.root);
    if (firmware > maxFirmware)
        return fail(QObject::tr("Cousoir writes to no Kobo with a firmware newer than Calibre writes to."));
    Changes changes;
    changes.identity = identityOf(input.root);
    SqliteDb db;
    QTemporaryDir copy;
    if (!Kobo::openDatabase(database, &db, &copy, error))
        return {};
    if (db.value(QStringLiteral("SELECT version FROM DbVersion")).toInt() > supportedDbVersion)
        return fail(QObject::tr("Cousoir writes to no Kobo with a database newer than Calibre writes to."));
    const QSet<QString> content = Kobo::columnsOf(db, QStringLiteral("content"));
    const QSet<QString> bookmark = Kobo::columnsOf(db, QStringLiteral("Bookmark"));
    for (const char *name : { "contentid", "contenttype", "bookid", "chapteridbookmarked", "___percentread", "datelastread",
                              "readstatus", "firsttimereading" }) {
        if (!content.contains(QLatin1String(name)))
            return fail(QObject::tr("This Kobo keeps its reading otherwise: Cousoir cannot write to it."));
    }
    for (const char *name : { "bookmarkid", "volumeid", "contentid", "startcontainerpath", "startcontainerchildindex",
                              "startoffset", "endcontainerpath", "endcontainerchildindex", "endoffset", "text", "annotation",
                              "datecreated", "datemodified", "chapterprogress", "hidden", "type" }) {
        if (!bookmark.contains(QLatin1String(name)))
            return fail(QObject::tr("This Kobo keeps its highlights otherwise: Cousoir cannot write to it."));
    }
    changes.hasColor = bookmark.contains(QStringLiteral("color"));
    changes.hasChapterProgress = content.contains(QStringLiteral("currentchapterprogress"));

    // What the Kobo has: its books put on it by hand, the files of each,
    // and the highlights of all.
    QHash<QString, KoboBook> books;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT ContentID, ReadStatus, DateLastRead FROM content "
                                                          "WHERE CAST(ContentType AS INTEGER) = 6 AND BookID IS NULL "
                                                          "AND ContentID LIKE 'file:///mnt/onboard/%'"))) {
        books.insert(row.value(0).toString(), { row.value(1).toInt(), KoboHighlights::koboTime(row.value(2).toString()) });
    }
    QHash<QString, QHash<QString, QString>> files;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT BookID, ContentID FROM content "
                                                          "WHERE CAST(ContentType AS INTEGER) = 9 "
                                                          "AND BookID LIKE 'file:///mnt/onboard/%'"))) {
        const QString file = KoboPlacement::fileOf(row.value(1).toString());
        if (!file.isEmpty())
            files[row.value(0).toString()].insert(file, row.value(1).toString());
    }
    QHash<QString, KoboMark> marks;
    for (const QVariantList &row :
         db.rows(QStringLiteral("SELECT BookmarkID, VolumeID, DateModified, DateCreated, Annotation, %1 FROM Bookmark")
                     .arg(changes.hasColor ? QStringLiteral("Color") : QStringLiteral("NULL")))) {
        QDateTime modified = KoboHighlights::koboTime(row.value(2).toString());
        if (!modified.isValid())
            modified = KoboHighlights::koboTime(row.value(3).toString());
        marks.insert(row.value(0).toString(), { row.value(1).toString(), modified,
                                                HighlightFormats::normalizedText(row.value(4).toString()),
                                                row.value(5).isNull() ? -1 : row.value(5).toInt() });
    }
    const QList<QVariantList> users = db.rows(QStringLiteral("SELECT UserID FROM user LIMIT 1"));
    changes.userId = users.isEmpty() ? QString() : users.first().value(0).toString();
    db.close();

    // What the library has: its highlights, the viewer's and the Kobo's own,
    // and Cousoir's reader's places.
    QHash<int, QList<Annotation>> annotations;
    {
        SqliteDb library;
        if (!library.open(input.snapshotDb, SqliteDb::Mode::Immutable, error))
            return {};
        for (const QVariantList &row :
             library.rows(QStringLiteral("SELECT book, format, annot_id, timestamp, annot_data, user_type, user FROM annotations "
                                         "WHERE annot_type='highlight' AND ((user_type='local' AND user='viewer') "
                                         "OR (user_type=? AND user=?))"),
                          { KoboSync::userType, input.device })) {
            annotations[row.value(0).toInt()].append({ row.value(1).toString().toUpper(), QStringLiteral("highlight"),
                                                       row.value(2).toString(), row.value(3).toDouble(),
                                                       row.value(4).toString().toUtf8(), QString(), row.value(5).toString(),
                                                       row.value(6).toString() });
        }
    }
    QJsonObject readerPlaces;
    {
        QFile file(input.positionsFile);
        if (!input.positionsFile.isEmpty() && file.open(QIODevice::ReadOnly))
            readerPlaces = QJsonDocument::fromJson(file.readAll()).object();
    }

    for (const DeviceBook &deviceBook : input.books) {
        const Book *book = input.data.book(deviceBook.bookId);
        if (!book || deviceBook.purchased || !deviceBook.contentId.startsWith(onboard)
            || !deviceBook.lpath.endsWith(QLatin1String(".kepub.epub"), Qt::CaseInsensitive))
            continue;
        const QString format = KoboPlacement::targetFormat(*book);
        QString path;
        for (const BookFormat &bookFormat : book->formats) {
            if (bookFormat.format == format)
                path = bookFormat.file;
        }
        if (path.isEmpty())
            continue;
        // Both copies, read once each when needed: the library's, and the
        // KEPUB the Kobo reads.
        std::unique_ptr<KoboPlacement::PlacementBook> library;
        std::unique_ptr<KoboPlacement::PlacementBook> kobo;
        bool opened = false;
        const auto open = [&] {
            if (!opened) {
                opened = true;
                QString ignored;
                library = KoboPlacement::PlacementBook::open(path, format, &ignored);
                kobo = KoboPlacement::PlacementBook::open(QDir(input.root).filePath(deviceBook.lpath), QStringLiteral("KEPUB"),
                                                          &ignored);
            }
            return library && kobo;
        };
        const auto koboBook = books.constFind(deviceBook.contentId);
        const auto skip = [&](const QString &why) { changes.skipped.append(QStringLiteral("%1: %2").arg(quoted(book->title), why)); };
        const QString notThere = QObject::tr("not on the Kobo's shelves yet: eject it once, then sync again");

        // Where reading stopped last, when newer than where the Kobo did:
        // the Kobo keeps its times to the second.
        std::optional<ReadPosition> newest;
        QList<ReadPosition> candidates = input.data.readPositions.value(book->id);
        const QJsonObject reader = readerPlaces.value(book->uuid).toObject();
        candidates.append({ reader.value(QLatin1String("format")).toString(), QStringLiteral("local"), QStringLiteral("cousoir"),
                            reader.value(QLatin1String("cfi")).toString(), reader.value(QLatin1String("epoch")).toDouble(),
                            reader.value(QLatin1String("fraction")).toDouble() });
        for (const ReadPosition &position : std::as_const(candidates)) {
            if (position.format.toUpper() == format && splitCfi(position.cfi) && (!newest || position.epoch > newest->epoch))
                newest = position;
        }
        const double koboRead = koboBook == books.cend() ? 0 : std::floor(secondsOf(koboBook->lastRead));
        if (newest && std::floor(newest->epoch) > koboRead) {
            const auto [spineIndex, cfi] = *splitCfi(newest->cfi);
            if (koboBook == books.cend()) {
                skip(notThere);
            } else if (!open()) {
                skip(QObject::tr("cannot be read"));
            } else {
                const std::optional<KoboPlacement::TextSpot> spot = library->textAt(spineIndex, cfi, QString());
                const std::optional<KoboPlacement::SpanRange> range = spot ? kobo->spansAt(*spot) : std::nullopt;
                if (range) {
                    changes.places.append({ deviceBook.contentId, book->title, range->spineName + u'#' + range->startSpan,
                                            int(std::lround(std::clamp(newest->fraction, 0.0, 1.0) * 100)), range->progress,
                                            KoboHighlights::koboTimeText(timeOf(std::floor(newest->epoch))),
                                            koboBook->status == 0 });
                } else {
                    skip(QObject::tr("where you stopped is not in the Kobo's copy"));
                }
            }
        }

        // The highlights: the library's own put there, the Kobo's own
        // changed here. Those put there before and gone were removed on
        // the Kobo, unless all are gone: the book was taken in again.
        bool keptSome = false;
        for (const Annotation &row : annotations.value(book->id)) {
            const auto mark = marks.constFind(KoboHighlights::bookmarkIdFor(row.id));
            keptSome = keptSome
                || (row.userType == QLatin1String("local") && mark != marks.cend() && mark->volumeId == deviceBook.contentId);
        }
        for (const Annotation &row : annotations.value(book->id)) {
            if (row.format != format)
                continue;
            const QJsonObject json = QJsonDocument::fromJson(row.data).object();
            const bool own = row.userType == QLatin1String("local");
            const QString id = own ? KoboHighlights::bookmarkIdFor(row.id) : row.id;
            const auto mark = marks.constFind(id);
            const bool there = mark != marks.cend() && mark->volumeId == deviceBook.contentId;
            if (json.value(QLatin1String("removed")).toBool()) {
                if (there)
                    changes.removed.append(id);
                continue;
            }
            const QString note = HighlightFormats::normalizedText(json.value(QLatin1String("notes")).toString());
            const int color = KoboHighlights::koboColor(json.value(QLatin1String("style")).toObject().value(QLatin1String("which")).toString());
            const QDateTime made = timeOf(row.timestamp);
            if (there) {
                if (std::floor(row.timestamp) > secondsOf(mark->modified)
                    && (note != mark->note || (changes.hasColor && color != mark->color))) {
                    Highlight changed;
                    changed.id = id;
                    changed.note = note;
                    changed.color = color;
                    changed.modified = KoboHighlights::koboTimeText(made);
                    changes.changed.append(changed);
                }
                continue;
            }
            // The Kobo's own, gone, or one put there and removed since.
            if (!own || (keptSome && input.sent.contains(id)))
                continue;
            if (koboBook == books.cend()) {
                skip(notThere);
                continue;
            }
            if (!open()) {
                skip(QObject::tr("cannot be read"));
                continue;
            }
            const std::optional<KoboPlacement::TextSpot> spot =
                library->textAt(json.value(QLatin1String("spine_index")).toInt(-1), json.value(QLatin1String("start_cfi")).toString(),
                                json.value(QLatin1String("end_cfi")).toString());
            const std::optional<KoboPlacement::SpanRange> range = spot ? kobo->spansAt(*spot) : std::nullopt;
            const QString file = range ? files.value(deviceBook.contentId).value(range->spineName) : QString();
            if (file.isEmpty()) {
                const QString text = quoted(json.value(QLatin1String("highlighted_text")).toString());
                skip(range ? QObject::tr("the Kobo does not know the file of the highlight %1").arg(text)
                           : QObject::tr("the highlight %1 is not in the Kobo's copy").arg(text));
                continue;
            }
            Highlight added;
            added.id = id;
            added.volumeId = deviceBook.contentId;
            added.contentId = file;
            added.startPath = KoboPlacement::spanPath(range->startSpan);
            added.startOffset = range->startOffset;
            added.endPath = KoboPlacement::spanPath(range->endSpan);
            added.endOffset = range->endOffset;
            added.text = json.value(QLatin1String("highlighted_text")).toString();
            added.note = note;
            added.created = KoboHighlights::koboCreatedText(made);
            added.modified = KoboHighlights::koboTimeText(made);
            added.chapterProgress = range->progress;
            added.color = color;
            changes.added.append(added);
        }
    }
    changes.skipped.removeDuplicates();
    return changes;
}

bool apply(const QString &root, const Changes &changes, const QString &backupFolder, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!Kobo::isKobo(root))
        return fail(QObject::tr("The Kobo is no longer there."));
    if (identityOf(root) != changes.identity)
        return fail(QObject::tr("The Kobo changed since it was read: sync again."));
    const QString database = databaseOf(root);

    // A copy first, the last few kept.
    const QDir folder(backupFolder);
    if (!QDir().mkpath(backupFolder))
        return fail(QObject::tr("Cannot keep a copy of the Kobo's database in %1.").arg(backupFolder));
    const QString backup = folder.filePath(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral(".sqlite"));
    for (const QString &suffix : { QString(), QStringLiteral("-wal") }) {
        QFile::remove(backup + suffix);
        if (QFileInfo::exists(database + suffix) && !QFile::copy(database + suffix, backup + suffix))
            return fail(QObject::tr("Cannot keep a copy of the Kobo's database in %1.").arg(backupFolder));
    }
    QStringList kept = folder.entryList({ QStringLiteral("*.sqlite") }, QDir::Files, QDir::Name);
    while (kept.size() > backups) {
        const QString oldest = folder.filePath(kept.takeFirst());
        QFile::remove(oldest);
        QFile::remove(oldest + QStringLiteral("-wal"));
    }

    SqliteDb db;
    QString message;
    if (!db.open(database, SqliteDb::Mode::ReadWrite, &message) || !db.exec(QStringLiteral("PRAGMA busy_timeout = 5000"), &message)
        || !db.exec(QStringLiteral("BEGIN IMMEDIATE"), &message))
        return fail(QObject::tr("Cannot write to the Kobo's database: %1").arg(message));
    const auto run = [&](const QString &sql, const QVariantList &values) { return db.run(sql, values, &message); };
    bool ok = true;
    for (const Place &place : changes.places) {
        if (!ok)
            break;
        // set_readstatus(): a book begun is no longer read the first time.
        ok = run(QStringLiteral("UPDATE content SET ChapterIDBookmarked = ?, ___PercentRead = ?, DateLastRead = ?%1%2 "
                                "WHERE ContentID = ? AND BookID IS NULL AND CAST(ContentType AS INTEGER) = 6")
                     .arg(changes.hasChapterProgress ? QStringLiteral(", CurrentChapterProgress = ?") : QString(),
                          place.begin ? QStringLiteral(", ReadStatus = 1, FirstTimeReading = 'false'") : QString()),
                 changes.hasChapterProgress
                     ? QVariantList{ place.bookmark, place.percent, place.lastRead, place.chapterProgress, place.contentId }
                     : QVariantList{ place.bookmark, place.percent, place.lastRead, place.contentId });
    }
    for (const Highlight &highlight : changes.added) {
        if (!ok)
            break;
        const QVariant note = highlight.note.isEmpty() ? QVariant() : QVariant(highlight.note);
        QVariantList values{ highlight.id,
                             highlight.volumeId,
                             highlight.contentId,
                             highlight.startPath,
                             highlight.startOffset,
                             highlight.endPath,
                             highlight.endOffset,
                             highlight.text,
                             note,
                             highlight.created,
                             highlight.chapterProgress,
                             highlight.modified,
                             changes.userId.isEmpty() ? QVariant() : QVariant(changes.userId),
                             highlight.note.isEmpty() ? QStringLiteral("highlight") : QStringLiteral("note") };
        if (changes.hasColor)
            values.append(highlight.color);
        ok = run(QStringLiteral("INSERT INTO Bookmark (BookmarkID, VolumeID, ContentID, StartContainerPath, "
                                "StartContainerChildIndex, StartOffset, EndContainerPath, EndContainerChildIndex, EndOffset, "
                                "Text, Annotation, DateCreated, ChapterProgress, Hidden, DateModified, UserID, Published, "
                                "Type%1) VALUES (?, ?, ?, ?, -99, ?, ?, -99, ?, ?, ?, ?, ?, 'false', ?, ?, 'false', ?%2)")
                     .arg(changes.hasColor ? QStringLiteral(", Color") : QString(), changes.hasColor ? QStringLiteral(", ?") : QString()),
                 values);
    }
    for (const Highlight &highlight : changes.changed) {
        if (!ok)
            break;
        QVariantList values{ highlight.note.isEmpty() ? QVariant() : QVariant(highlight.note), highlight.note.isEmpty() ? QStringLiteral("highlight") : QStringLiteral("note"),
                             highlight.modified };
        if (changes.hasColor)
            values.append(highlight.color);
        values.append(highlight.id);
        ok = run(QStringLiteral("UPDATE Bookmark SET Annotation = ?, Type = CASE WHEN Type IN ('highlight', 'note') THEN ? ELSE Type END, "
                                "DateModified = ?%1 WHERE BookmarkID = ?")
                     .arg(changes.hasColor ? QStringLiteral(", Color = ?") : QString()),
                 values);
    }
    for (const QString &id : changes.removed) {
        if (!ok)
            break;
        ok = run(QStringLiteral("DELETE FROM Bookmark WHERE BookmarkID = ?"), { id });
    }
    if (!ok || !db.exec(QStringLiteral("COMMIT"), &message)) {
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        return fail(QObject::tr("Cannot write to the Kobo's database: %1").arg(message));
    }
    // copy_db(): the log taken into the database, for the Kobo to find it
    // whole.
    if (db.value(QStringLiteral("PRAGMA journal_mode")).toString().compare(QLatin1String("wal"), Qt::CaseInsensitive) == 0)
        db.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), nullptr);
    db.close();
    return true;
}

Result sync(const Input &input)
{
    Result result;
    const Changes changes = plan(input, &result.error);
    if (!result.error.isEmpty())
        return result;
    result.skipped = changes.skipped;
    if (!changes.isEmpty()) {
        if (!apply(input.root, changes, backupFolder(input.device), &result.error))
            return result;
        QHash<QString, QDateTime> sent = input.sent;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (const Highlight &highlight : changes.added)
            sent.insert(highlight.id, now);
        for (const QString &id : changes.removed)
            sent.remove(id);
        QString ignored;
        KoboHighlights::writeSent(input.device, sent, &ignored);
    }
    result.ok = true;
    result.places = int(changes.places.size());
    result.highlights = int(changes.added.size() + changes.changed.size() + changes.removed.size());
    return result;
}

} // namespace KoboWrite
