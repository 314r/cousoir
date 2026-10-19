#include "book_edit.h"

#include <QFile>
#include <QObject>
#include <QUrl>

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
