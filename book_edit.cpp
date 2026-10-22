// SPDX-License-Identifier: GPL-3.0-only
//
// BulkEdit is ported from Calibre, https://calibre-ebook.com, Copyright (C)
// Kovid Goyal, GPL v3:
// - src/calibre/gui2/dialogs/metadata_bulk.py: MyBlockingBusy.do_all()
// - src/calibre/db/legacy.py: cleanup_tags(), _do_bulk_modify()
// - src/calibre/db/cache.py: get_next_series_num_for()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "book_edit.h"

#include <QFile>
#include <QSet>
#include <QObject>
#include <QUrl>

#include "book.h"

namespace {

// A list from QML or JSON: an array, or text split on `separator`.
std::optional<QStringList> listValue(const QVariant &value, QChar separator)
{
    if (value.typeId() == QMetaType::QString) {
        QStringList parts = value.toString().split(separator);
        for (QString &part : parts)
            part = part.trimmed();
        parts.removeAll(QString());
        return parts;
    }
    if (value.canConvert<QVariantList>()) {
        QStringList result;
        for (const QVariant &item : value.toList())
            result.append(item.toString());
        return result;
    }
    return std::nullopt;
}

} // namespace

bool BookEdit::isEmpty() const
{
    return !title && !sort && !authors && !authorSort && !series && !seriesIndex && !tags && !rating && !publisher
        && !languages && !identifiers && !pubdate && !comments && !cover;
}

bool BookEdit::movesFiles() const
{
    return title.has_value() || authors.has_value();
}

bool BookEdit::fromVariantMap(int bookId, const QVariantMap &changes, BookEdit *edit, QString *error)
{
    BookEdit result;
    result.bookId = bookId;
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        const QString &key = it.key();
        const QVariant &value = it.value();
        if (key == QLatin1String("title")) {
            result.title = value.toString();
        } else if (key == QLatin1String("sort")) {
            result.sort = value.toString();
        } else if (key == QLatin1String("authors")) {
            result.authors = listValue(value, u'&');
            if (!result.authors)
                return fail(QObject::tr("The authors must be a list or a text."));
        } else if (key == QLatin1String("authorSort")) {
            result.authorSort = value.toString();
        } else if (key == QLatin1String("series")) {
            result.series = value.toString();
        } else if (key == QLatin1String("seriesIndex")) {
            bool ok = false;
            const double index = value.toString().trimmed().replace(u',', u'.').toDouble(&ok);
            if (!ok || !qIsFinite(index) || index < 0)
                return fail(QObject::tr("“%1” is not a series number.").arg(value.toString()));
            result.seriesIndex = index;
        } else if (key == QLatin1String("tags")) {
            result.tags = listValue(value, u',');
            if (!result.tags)
                return fail(QObject::tr("The tags must be a list or a text."));
        } else if (key == QLatin1String("rating")) {
            bool ok = false;
            const int rating = value.toInt(&ok);
            if (!ok || rating < 0 || rating > 10)
                return fail(QObject::tr("A rating goes from 0 to 10."));
            result.rating = rating;
        } else if (key == QLatin1String("publisher")) {
            result.publisher = value.toString();
        } else if (key == QLatin1String("languages")) {
            result.languages = listValue(value, u',');
            if (!result.languages)
                return fail(QObject::tr("The languages must be a list or a text."));
        } else if (key == QLatin1String("identifiers")) {
            QList<QPair<QString, QString>> identifiers;
            if (value.typeId() == QMetaType::QString) {
                for (const QString &part : value.toString().split(u',', Qt::SkipEmptyParts))
                    identifiers.append({ part.section(u':', 0, 0).trimmed(), part.section(u':', 1).trimmed() });
            } else if (value.canConvert<QVariantMap>()) {
                const QVariantMap map = value.toMap();
                for (auto identifier = map.constBegin(); identifier != map.constEnd(); ++identifier)
                    identifiers.append({ identifier.key(), identifier.value().toString() });
            } else {
                return fail(QObject::tr("The identifiers must be a map or a text."));
            }
            result.identifiers = identifiers;
        } else if (key == QLatin1String("pubdate")) {
            result.pubdate = value.toString();
        } else if (key == QLatin1String("comments")) {
            result.comments = value.toString();
        } else if (key == QLatin1String("coverFile")) {
            QString path = value.toString();
            if (path.startsWith(QLatin1String("file://")))
                path = QUrl(path).toLocalFile();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return fail(QObject::tr("Cannot read %1: %2").arg(path, file.errorString()));
            result.cover = file.readAll();
            if (result.cover->isEmpty())
                return fail(QObject::tr("%1 is empty.").arg(path));
        } else if (key == QLatin1String("removeCover")) {
            if (value.toBool())
                result.cover = QByteArray();
        } else {
            return fail(QObject::tr("Unknown field “%1”.").arg(key));
        }
    }
    *edit = result;
    return true;
}

bool BulkEdit::isEmpty() const
{
    return bookIds.isEmpty()
        || (!authors && !rating && !publisher && !series && !languages && !pubdate && !removeAllTags
            && addTags.isEmpty() && removeTags.isEmpty());
}

bool BulkEdit::fromVariantMap(const QVariantMap &changes, BulkEdit *edit, QString *error)
{
    BulkEdit result;
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto number = [](const QVariant &value, double *out) {
        bool ok = false;
        *out = value.toString().trimmed().replace(u',', u'.').toDouble(&ok);
        return ok && qIsFinite(*out);
    };
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        const QString &key = it.key();
        const QVariant &value = it.value();
        if (key == QLatin1String("books")) {
            for (const QVariant &id : value.toList())
                result.bookIds.append(id.toInt());
        } else if (key == QLatin1String("authors")) {
            result.authors = listValue(value, u'&');
            if (!result.authors)
                return fail(QObject::tr("The authors must be a list or a text."));
        } else if (key == QLatin1String("rating")) {
            bool ok = false;
            const int rating = value.toInt(&ok);
            if (!ok || rating < 0 || rating > 10)
                return fail(QObject::tr("A rating goes from 0 to 10."));
            result.rating = rating;
        } else if (key == QLatin1String("publisher")) {
            result.publisher = value.toString().trimmed();
        } else if (key == QLatin1String("series")) {
            result.series = value.toString().trimmed();
        } else if (key == QLatin1String("numberSeries")) {
            result.numberSeries = value.toBool();
        } else if (key == QLatin1String("seriesStart")) {
            double start = 0;
            if (!number(value, &start) || start < 0)
                return fail(QObject::tr("“%1” is not a series number.").arg(value.toString()));
            result.seriesStart = start;
        } else if (key == QLatin1String("seriesIncrement")) {
            if (!number(value, &result.seriesIncrement) || result.seriesIncrement < 0)
                return fail(QObject::tr("“%1” is not a series number.").arg(value.toString()));
        } else if (key == QLatin1String("languages")) {
            result.languages = listValue(value, u',');
            if (!result.languages)
                return fail(QObject::tr("The languages must be a list or a text."));
        } else if (key == QLatin1String("pubdate")) {
            result.pubdate = value.toString();
        } else if (key == QLatin1String("removeAllTags")) {
            result.removeAllTags = value.toBool();
        } else if (key == QLatin1String("addTags") || key == QLatin1String("removeTags")) {
            const std::optional<QStringList> tags = listValue(value, u',');
            if (!tags)
                return fail(QObject::tr("The tags must be a list or a text."));
            (key == QLatin1String("addTags") ? result.addTags : result.removeTags) = *tags;
        } else {
            return fail(QObject::tr("Unknown field “%1”.").arg(key));
        }
    }
    *edit = result;
    return true;
}

namespace {

// cleanup_tags(): trimmed, commas as semicolons, spaces collapsed, each tag
// once whatever its case.
QStringList cleanupTags(const QStringList &tags)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &tag : tags) {
        const QString cleaned = CalibreText::collapseSpaces(tag.trimmed().replace(u',', u';'));
        if (cleaned.isEmpty() || seen.contains(cleaned.toLower()))
            continue;
        seen.insert(cleaned.toLower());
        result.append(cleaned);
    }
    return result;
}

} // namespace

QList<BookEdit> BulkEdit::edits(const LibraryData &data, const CalibreText::Tweaks &tweaks) const
{
    QList<const Book *> books;
    for (const int id : bookIds) {
        if (const Book *book = data.book(id))
            books.append(book);
    }

    // The series numbers, in the books' order.
    QHash<int, double> seriesIndices;
    if (series && !series->isEmpty() && numberSeries) {
        if (seriesStart) {
            for (qsizetype i = 0; i < books.size(); ++i)
                seriesIndices.insert(books.at(i)->id, *seriesStart + double(i) * seriesIncrement);
        } else {
            // get_next_series_num_for(current_indices=True): the books of the
            // series by name, case aside, each then numbered after them all.
            QHash<int, double> current;
            const QString name = series->toLower();
            for (const Book &book : data.books) {
                if (book.seriesId > 0 && book.series.toLower() == name)
                    current.insert(book.id, book.seriesIndex);
            }
            for (const Book *book : std::as_const(books)) {
                QList<double> indices = current.values();
                std::sort(indices.begin(), indices.end());
                const double next = CalibreText::nextSeriesIndex(indices, tweaks);
                current.insert(book->id, next);
                seriesIndices.insert(book->id, next);
            }
        }
    }

    // bulk_modify_tags(): the tags removed are those not added too, case
    // aside; a tag added keeps the library's spelling of it.
    const QStringList added = cleanupTags(addTags);
    QSet<QString> removed;
    for (const QString &tag : cleanupTags(removeTags))
        removed.insert(tag.toLower());
    for (const QString &tag : added)
        removed.remove(tag.toLower());
    QHash<QString, QString> spelling;
    for (const FacetItem &tag : data.tags)
        spelling.insert(tag.name.toLower(), tag.name);
    const bool tagsChange = removeAllTags || !added.isEmpty() || !removed.isEmpty();

    QList<BookEdit> result;
    for (const Book *book : std::as_const(books)) {
        BookEdit edit;
        edit.bookId = book->id;
        edit.authors = authors;
        edit.rating = rating;
        edit.publisher = publisher;
        if (series) {
            edit.series = *series;
            if (series->isEmpty())
                edit.seriesIndex = 1.0;
            else if (seriesIndices.contains(book->id))
                edit.seriesIndex = seriesIndices.value(book->id);
            else if (tweaks.seriesIndexAutoIncrement != QLatin1String("no_change"))
                edit.seriesIndex = 1.0;
        }
        edit.pubdate = pubdate;
        edit.languages = languages;
        if (tagsChange) {
            QStringList tags = removeAllTags ? QStringList() : book->tags;
            if (!added.isEmpty() || !removed.isEmpty()) {
                QSet<QString> existing;
                for (const QString &tag : std::as_const(tags))
                    existing.insert(tag.toLower());
                for (const QString &tag : added) {
                    if (!existing.contains(tag.toLower()))
                        tags.append(spelling.value(tag.toLower(), tag));
                }
                tags.removeIf([&removed](const QString &tag) { return removed.contains(tag.toLower()); });
            }
            edit.tags = tags;
        }
        result.append(edit);
    }
    return result;
}
