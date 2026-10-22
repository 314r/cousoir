// The sorts of the golden test: Cousoir's book list sorted as
// tools/oracle/sort_oracle.py sorts Calibre's, and both compared.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <numeric>

#include "book_sort.h"
#include "calibre_config.h"
#include "calibre_db.h"
#include "common.h"

namespace Tool {

int sortBooks(const QStringList &arguments)
{
    const QString library = arguments.at(0);
    const std::optional<QJsonValue> sorts = readJson(arguments.at(1));
    if (!sorts)
        return 1;
    const CalibreConfig config = CalibreConfig::load();
    LibraryData data;
    QString error;
    QTemporaryDir cache;
    if (!CalibreDb::loadLibrary(library, cache.filePath(QStringLiteral("snapshot.db")), config, &data, &error)) {
        err() << library << ": " << error << Qt::endl;
        return 1;
    }
    const BookSort::Sorter sorter(data, config);
    QJsonArray results;
    for (const QJsonValue &sort : sorts->toArray()) {
        QList<BookSort::Level> levels;
        for (const QJsonValue &level : sort.toArray())
            levels.append({ level.toArray().at(0).toString(), level.toArray().at(1).toBool() });
        // The books by id, as Calibre's list has them before any sort.
        QList<qsizetype> rows(data.books.size());
        std::iota(rows.begin(), rows.end(), qsizetype(0));
        std::sort(rows.begin(), rows.end(),
                  [&data](qsizetype a, qsizetype b) { return data.books.at(a).id < data.books.at(b).id; });
        sorter.sort(levels, &rows);
        QJsonArray ids;
        for (const qsizetype row : std::as_const(rows))
            ids.append(data.books.at(row).id);
        results.append(QJsonObject{ { QStringLiteral("sort"), sort }, { QStringLiteral("ids"), ids } });
    }
    return writeFile(arguments.at(2), QJsonDocument(results).toJson()) ? 0 : 1;
}

int compareSorts(const QStringList &arguments)
{
    const std::optional<QJsonValue> calibre = readJson(arguments.at(0));
    const std::optional<QJsonValue> cousoir = readJson(arguments.at(1));
    if (!calibre || !cousoir)
        return 1;
    const QJsonArray expected = calibre->toArray();
    const QJsonArray actual = cousoir->toArray();
    if (expected.size() != actual.size()) {
        out() << "Calibre ran " << expected.size() << " sorts, Cousoir " << actual.size() << Qt::endl;
        return 1;
    }
    int differences = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        const QJsonObject a = expected.at(i).toObject();
        const QJsonObject b = actual.at(i).toObject();
        const QString sort = QString::fromUtf8(QJsonDocument(a.value(QLatin1String("sort")).toArray()).toJson(QJsonDocument::Compact));
        if (a.contains(QLatin1String("error"))) {
            out() << "  " << sort << ": not compared, Calibre fails with " << a.value(QLatin1String("error")).toString()
                  << Qt::endl;
            continue;
        }
        const QJsonArray idsA = a.value(QLatin1String("ids")).toArray();
        const QJsonArray idsB = b.value(QLatin1String("ids")).toArray();
        if (idsA == idsB)
            continue;
        ++differences;
        qsizetype at = 0;
        while (at < idsA.size() && at < idsB.size() && idsA.at(at) == idsB.at(at))
            ++at;
        out() << "  " << sort << ": first difference at " << at << ", Calibre "
              << display(QJsonArray::fromVariantList(idsA.toVariantList().mid(at, 8))) << ", Cousoir "
              << display(QJsonArray::fromVariantList(idsB.toVariantList().mid(at, 8))) << Qt::endl;
    }
    out() << expected.size() - differences << " of " << expected.size() << " sorts alike" << Qt::endl;
    return differences == 0 ? 0 : 1;
}

} // namespace Tool
