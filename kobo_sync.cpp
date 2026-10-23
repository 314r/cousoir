// SPDX-License-Identifier: GPL-3.0-only

#include "kobo_sync.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <memory>

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

} // namespace

Plan plan(const Input &input)
{
    Plan plan;
    const KoboHighlights::Snapshot &snapshot = input.snapshot;
    const bool live = snapshot.source == QLatin1String("usb");
    const double now = double(QDateTime::currentMSecsSinceEpoch()) / 1000;
    const QList<int> ids = KoboHighlights::match(snapshot.books, input.data);

    // The library's annotations of the Kobo's user, by book.
    QHash<int, QList<Annotation>> rows;
    {
        SqliteDb db;
        QString error;
        if (db.open(input.snapshotDb, SqliteDb::Mode::Immutable, &error)) {
            for (const QVariantList &row :
                 db.rows(QStringLiteral("SELECT book, format, annot_type, annot_id, timestamp, annot_data, searchable_text "
                                        "FROM annotations WHERE user_type=? AND user=? AND annot_type='highlight'"),
                         { userType, snapshot.device })) {
                rows[row.value(0).toInt()].append({ row.value(1).toString(), row.value(2).toString(), row.value(3).toString(),
                                                    row.value(4).toDouble(), row.value(5).toString().toUtf8(),
                                                    row.value(6).toString(), userType, snapshot.device });
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
        QStringList present;
        for (const KoboHighlights::Highlight &highlight : kobo.highlights) {
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
                if (!opened) {
                    opened = true;
                    QString error;
                    placing = KoboPlacement::PlacementBook::open(file, format, &error);
                }
                if (!placing) {
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
    }
    for (const int bookId : std::as_const(order)) {
        const DeviceAnnotations &set = sets.value(bookId);
        if (!BookOperations::mergeSteps(rows.value(bookId), set, now).isEmpty())
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
