// SPDX-License-Identifier: GPL-3.0-only

#include "kobo_sync.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <tuple>

#include "book_operations.h"
#include "highlight_json.h"
#include "kobo_placement.h"
#include "sqlite_db.h"

namespace KoboSync {

namespace {

// Where a highlight was found, by what it is and the file it was found in:
// once placed, a highlight is not looked for again until either changes.
QString cacheKey(const KoboHighlights::Highlight &highlight, const QString &file)
{
    const QFileInfo info(file);
    const QStringList parts{ highlight.id,
                             highlight.modified,
                             highlight.created,
                             highlight.text,
                             highlight.contentId,
                             highlight.startPath,
                             QString::number(highlight.startOffset),
                             highlight.endPath,
                             QString::number(highlight.endOffset),
                             info.absoluteFilePath(),
                             QString::number(info.size()),
                             QString::number(info.lastModified().toMSecsSinceEpoch()) };
    return QString::fromLatin1(
        QCryptographicHash::hash(parts.join(QChar(0x1f)).toUtf8(), QCryptographicHash::Sha1).toHex());
}

QJsonObject placementJson(const KoboPlacement::Placement &placement)
{
    return { { QStringLiteral("method"), KoboPlacement::methodName(placement.method) },
             { QStringLiteral("spine_index"), placement.spineIndex },
             { QStringLiteral("spine_name"), placement.spineName },
             { QStringLiteral("start_cfi"), placement.startCfi },
             { QStringLiteral("end_cfi"), placement.endCfi },
             { QStringLiteral("toc_family_titles"), QJsonArray::fromStringList(placement.tocFamily) },
             { QStringLiteral("reason"), placement.reason } };
}

KoboPlacement::Placement placementOf(const QJsonObject &json)
{
    KoboPlacement::Placement placement;
    const QString method = json.value(QLatin1String("method")).toString();
    placement.method = method == QLatin1String("spans") ? KoboPlacement::Method::Spans
        : method == QLatin1String("text")              ? KoboPlacement::Method::Text
        : method == QLatin1String("book")              ? KoboPlacement::Method::Book
                                                       : KoboPlacement::Method::None;
    placement.spineIndex = json.value(QLatin1String("spine_index")).toInt(-1);
    placement.spineName = json.value(QLatin1String("spine_name")).toString();
    placement.startCfi = json.value(QLatin1String("start_cfi")).toString();
    placement.endCfi = json.value(QLatin1String("end_cfi")).toString();
    for (const QJsonValue &title : json.value(QLatin1String("toc_family_titles")).toArray())
        placement.tocFamily.append(title.toString());
    placement.reason = json.value(QLatin1String("reason")).toString();
    return placement;
}

bool isRemoved(const Annotation &annotation)
{
    return QJsonDocument::fromJson(annotation.data).object().value(QLatin1String("removed")).toBool();
}

// A highlight of the library's own put on the Kobo, as the Kobo has it
// now: removed, or its note or colour changed there since; none when it
// is as the library has it.
std::optional<Annotation> changedOnKobo(const Annotation &row, const KoboHighlights::Highlight &highlight)
{
    const QDateTime changed = KoboPlacement::changedAt(highlight);
    if (isRemoved(row) || !changed.isValid())
        return std::nullopt;
    const double at = double(changed.toMSecsSinceEpoch()) / 1000;
    if (highlight.hidden) {
        return Annotation{ row.format,
                           row.type,
                           row.id,
                           at,
                           QJsonDocument(HighlightJson::removed(row.id, HighlightJson::isoTimestamp(changed))).toJson(QJsonDocument::Compact),
                           QString(),
                           row.userType,
                           row.user };
    }
    // The Kobo keeps its times to the second.
    if (at < row.timestamp + 1)
        return std::nullopt;
    QJsonObject json = QJsonDocument::fromJson(row.data).object();
    const QJsonObject before = json;
    if (highlight.note.isEmpty())
        json.remove(QStringLiteral("notes"));
    else
        json.insert(QStringLiteral("notes"), highlight.note);
    // A colour the Kobo has not went as yellow: changed only when the Kobo's
    // is another.
    QJsonObject style = json.value(QLatin1String("style")).toObject();
    if (highlight.color >= 0 && KoboHighlights::koboColor(style.value(QLatin1String("which")).toString()) != highlight.color) {
        style.insert(QStringLiteral("which"), KoboHighlights::calibreColor(highlight.color));
        json.insert(QStringLiteral("style"), style);
    }
    if (json == before)
        return std::nullopt;
    json.insert(QStringLiteral("timestamp"), HighlightJson::isoTimestamp(changed));
    return Annotation{ row.format, row.type, row.id, at, QJsonDocument(json).toJson(QJsonDocument::Compact),
                       HighlightJson::searchableText(json), row.userType, row.user };
}

} // namespace

Plan plan(const Input &input)
{
    Plan plan;
    const KoboHighlights::Snapshot &snapshot = input.snapshot;
    const bool live = snapshot.source == QLatin1String("usb");
    const double now = double(QDateTime::currentMSecsSinceEpoch()) / 1000;
    const QList<int> ids = KoboHighlights::match(snapshot.books, input.data);

    // The library's annotations of the Kobo's user, and its viewer's, by
    // book.
    QHash<int, QList<Annotation>> rows;
    QHash<int, QList<Annotation>> viewerRows;
    {
        SqliteDb db;
        QString error;
        if (db.open(input.snapshotDb, SqliteDb::Mode::Immutable, &error)) {
            const QString sql = QStringLiteral("SELECT book, format, annot_type, annot_id, timestamp, annot_data, searchable_text "
                                               "FROM annotations WHERE user_type=? AND user=? AND annot_type='highlight'");
            for (const auto &[into, type, user] : { std::tuple(&rows, userType, snapshot.device),
                                                    std::tuple(&viewerRows, QStringLiteral("local"), QStringLiteral("viewer")) }) {
                for (const QVariantList &row : db.rows(sql, { type, user })) {
                    (*into)[row.value(0).toInt()].append({ row.value(1).toString(), row.value(2).toString(),
                                                           row.value(3).toString(), row.value(4).toDouble(),
                                                           row.value(5).toString().toUtf8(), row.value(6).toString(), type,
                                                           user });
                }
            }
        }
    }
    QJsonObject cache;
    if (!input.cacheFile.isEmpty()) {
        QFile file(input.cacheFile);
        if (file.open(QIODevice::ReadOnly))
            cache = QJsonDocument::fromJson(file.readAll()).object();
    }
    bool cacheChanged = false;
    // By library book: the Kobo may have it twice, a KEPUB and an EPUB, all
    // of whose highlights it has.
    QHash<int, DeviceAnnotations> sets;
    QList<int> order;
    // The library's own highlights on the Kobo: what became of them there,
    // by book and format, and those it still has.
    QMap<QPair<int, QString>, DeviceAnnotations> viewerSets;
    QHash<int, QSet<QString>> seenOwn;
    QSet<int> onKobo;
    QHash<int, ReadPosition> positions;
    const auto viewerSet = [&viewerSets](int bookId, const QString &format) -> DeviceAnnotations & {
        auto found = viewerSets.find({ bookId, format });
        if (found == viewerSets.end())
            found = viewerSets.insert({ bookId, format }, { bookId, format, QStringLiteral("local"), QStringLiteral("viewer"), {}, std::nullopt });
        return *found;
    };

    for (qsizetype i = 0; i < snapshot.books.size(); ++i) {
        const KoboHighlights::Book &kobo = snapshot.books.at(i);
        const Book *libraryBook = input.data.book(ids.at(i));
        Left left{ kobo, libraryBook ? libraryBook->id : 0, {} };
        left.book.highlights.clear();
        const auto leave = [&](const KoboHighlights::Highlight &highlight, const QString &reason) {
            left.book.highlights.append(highlight);
            left.reasons.insert(highlight.id, reason);
            ++plan.leftOut;
        };
        if (!libraryBook) {
            for (const KoboHighlights::Highlight &highlight : kobo.highlights) {
                if (!highlight.hidden)
                    leave(highlight, QStringLiteral("not in the library"));
            }
            if (!left.book.highlights.isEmpty())
                plan.left.append(left);
            continue;
        }
        const QString format = KoboPlacement::targetFormat(*libraryBook);
        QString file;
        for (const BookFormat &bookFormat : libraryBook->formats) {
            if (bookFormat.format == format)
                file = bookFormat.file;
        }
        QHash<QString, const Annotation *> existing;
        for (const Annotation &row : std::as_const(rows[libraryBook->id]))
            existing.insert(row.id, &row);
        if (!sets.contains(libraryBook->id)) {
            order.append(libraryBook->id);
            sets.insert(libraryBook->id, { libraryBook->id, format.isEmpty() ? QStringLiteral("EPUB") : format, userType,
                                           snapshot.device, {}, QStringList() });
        }
        DeviceAnnotations &set = sets[libraryBook->id];
        std::unique_ptr<KoboPlacement::PlacementBook> placing;
        bool opened = false;
        const auto openPlacing = [&] {
            if (!opened) {
                opened = true;
                QString error;
                placing = KoboPlacement::PlacementBook::open(file, format, &error);
            }
            return placing.get();
        };
        QHash<QString, const Annotation *> own;
        for (const Annotation &row : std::as_const(viewerRows[libraryBook->id]))
            own.insert(KoboHighlights::bookmarkIdFor(row.id), &row);
        if (live && kobo.onDevice)
            onKobo.insert(libraryBook->id);
        QStringList present;
        for (const KoboHighlights::Highlight &highlight : kobo.highlights) {
            if (const Annotation *row = own.value(highlight.id)) {
                seenOwn[libraryBook->id].insert(highlight.id);
                if (const std::optional<Annotation> change = changedOnKobo(*row, highlight))
                    viewerSet(libraryBook->id, row->format).annotations.append(*change);
                continue;
            }
            const QDateTime changed = KoboPlacement::changedAt(highlight);
            const double at = changed.isValid() ? double(changed.toMSecsSinceEpoch()) / 1000 : now;
            const Annotation *row = existing.value(highlight.id);
            const bool removedHere = row && isRemoved(*row);
            if (highlight.hidden) {
                if (row && !removedHere) {
                    const QJsonObject removed = HighlightJson::removed(
                        highlight.id, HighlightJson::isoTimestamp(changed.isValid() ? changed : QDateTime::currentDateTimeUtc()));
                    set.annotations.append({ set.format, QStringLiteral("highlight"), highlight.id, at,
                                             QJsonDocument(removed).toJson(QJsonDocument::Compact), QString(), userType,
                                             snapshot.device });
                }
                continue;
            }
            present.append(highlight.id);
            // Removed in the library, or as the Kobo has it, or changed in
            // the library since.
            if (removedHere)
                continue;
            if (row && row->timestamp >= at) {
                ++plan.kept;
                continue;
            }
            if (format.isEmpty()) {
                leave(highlight, QStringLiteral("no EPUB in the library"));
                continue;
            }
            const QString key = cacheKey(highlight, file);
            KoboPlacement::Placement placement;
            if (cache.contains(key)) {
                placement = placementOf(cache.value(key).toObject());
            } else {
                if (!openPlacing()) {
                    leave(highlight, QStringLiteral("book not readable"));
                    continue;
                }
                placement = placing->place(highlight);
                cache.insert(key, placementJson(placement));
                cacheChanged = true;
            }
            if (placement.method == KoboPlacement::Method::None) {
                leave(highlight, placement.reason);
                continue;
            }
            const QJsonObject json = KoboPlacement::annotation(highlight, placement);
            set.annotations.append({ format, QStringLiteral("highlight"), highlight.id, at,
                                     QJsonDocument(json).toJson(QJsonDocument::Compact), HighlightJson::searchableText(json),
                                     userType, snapshot.device });
            ++plan.placed;
        }
        // All it has only when each copy of the book says all it has.
        if (live && kobo.onDevice && set.present)
            *set.present += present;
        else
            set.present.reset();
        if (!left.book.highlights.isEmpty())
            plan.left.append(left);

        // Where it stopped, read on the Kobo since the library last heard
        // of the book's reading: the Kobo keeps its times to the second.
        const QDateTime read = kobo.place ? KoboHighlights::koboTime(kobo.place->lastRead) : QDateTime();
        if (read.isValid() && !format.isEmpty()) {
            const double at = double(read.toMSecsSinceEpoch()) / 1000;
            double newest = positions.contains(libraryBook->id) ? positions.value(libraryBook->id).epoch : 0;
            for (const ReadPosition &position : input.data.readPositions.value(libraryBook->id))
                newest = std::max(newest, std::floor(position.epoch));
            if (at > newest && openPlacing()) {
                const KoboPlacement::Spot spot = placing->placePoint(kobo.place->bookmark, kobo.place->chapterProgress);
                if (spot.method != KoboPlacement::Method::None) {
                    positions.insert(libraryBook->id,
                                     { format, userType, snapshot.device,
                                       QStringLiteral("epubcfi(/%1%2)").arg(2 * (spot.spineIndex + 1)).arg(spot.cfi), at,
                                       std::clamp(kobo.place->percent, 0, 100) / 100.0 });
                }
            }
        }
    }
    for (const int bookId : std::as_const(order)) {
        const DeviceAnnotations &set = sets.value(bookId);
        if (!BookOperations::mergeSteps(rows.value(bookId), set, now).isEmpty())
            plan.changes.append(set);
        if (positions.contains(bookId))
            plan.positions.append({ bookId, positions.value(bookId) });
        // Put there and gone since: removed on the Kobo, which deletes them;
        // unless all are gone, the book taken in again rather.
        if (!onKobo.contains(bookId) || seenOwn.value(bookId).isEmpty())
            continue;
        for (const Annotation &row : viewerRows.value(bookId)) {
            const QString id = KoboHighlights::bookmarkIdFor(row.id);
            const QDateTime sent = input.sent.value(id);
            if (!seenOwn.value(bookId).contains(id) && sent.isValid() && snapshot.seen > sent && !isRemoved(row)) {
                viewerSet(bookId, row.format)
                    .annotations.append({ row.format, row.type, row.id, now,
                                          QJsonDocument(HighlightJson::removed(row.id, HighlightJson::isoTimestamp(snapshot.seen)))
                                              .toJson(QJsonDocument::Compact),
                                          QString(), row.userType, row.user });
            }
        }
    }
    for (const DeviceAnnotations &set : std::as_const(viewerSets)) {
        if (!BookOperations::mergeSteps(viewerRows.value(set.bookId), set, now).isEmpty())
            plan.changes.append(set);
    }

    if (cacheChanged && !input.cacheFile.isEmpty()) {
        QDir().mkpath(QFileInfo(input.cacheFile).path());
        QSaveFile file(input.cacheFile);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(cache).toJson(QJsonDocument::Compact));
            file.commit();
        }
    }
    return plan;
}

} // namespace KoboSync
