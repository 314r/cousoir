// The searches of the golden test: Cousoir's run on a library as
// tools/oracle/search_books.py runs Calibre's, and both compared.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "calibre_config.h"
#include "calibre_db.h"
#include "calibre_search.h"
#include "common.h"

namespace Tool {

int searchBooks(const QStringList &arguments)
{
    const QString library = arguments.at(0);
    QFile queries(arguments.at(1));
    if (!queries.open(QIODevice::ReadOnly)) {
        err() << arguments.at(1) << ": " << queries.errorString() << Qt::endl;
        return 1;
    }
    const CalibreConfig config = CalibreConfig::load();
    LibraryData data;
    QString error;
    QTemporaryDir cache;
    if (!CalibreDb::loadLibrary(library, cache.filePath(QStringLiteral("snapshot.db")), config, &data, &error)) {
        err() << library << ": " << error << Qt::endl;
        return 1;
    }
    const CalibreSearch::Index index(data, CalibreSearch::Preferences::fromRows(data.preferences), config.tweaks,
                                     config.interfaceLanguage);

    QJsonArray results;
    QString restriction;
    for (const QString &line : QString::fromUtf8(queries.readAll()).split(u'\n')) {
        if (line.trimmed().isEmpty() || line.startsWith(QLatin1String("# ")))
            continue;
        if (line.startsWith(QLatin1String("@vl"))) {
            const QString name = line.mid(3).trimmed();
            restriction = name.isEmpty() ? QString() : index.preferences().virtualLibraries.value(name);
            continue;
        }
        QJsonObject entry{ { QStringLiteral("query"), line }, { QStringLiteral("restriction"), restriction } };
        const CalibreSearch::Result result = CalibreSearch::search(index, line, restriction);
        if (result.ok()) {
            QJsonArray ids;
            for (qsizetype row = 0; row < result.rows.size(); ++row) {
                if (result.rows.testBit(row))
                    ids.append(data.books.at(row).id);
            }
            entry.insert(QStringLiteral("ids"), ids);
        } else {
            entry.insert(QStringLiteral("error"), true);
        }
        results.append(entry);
    }
    return writeFile(arguments.at(2), QJsonDocument(results).toJson()) ? 0 : 1;
}

int compareSearches(const QStringList &arguments)
{
    const std::optional<QJsonValue> calibre = readJson(arguments.at(0));
    const std::optional<QJsonValue> cousoir = readJson(arguments.at(1));
    if (!calibre || !cousoir)
        return 1;
    const QJsonArray expected = calibre->toArray();
    const QJsonArray actual = cousoir->toArray();
    if (expected.size() != actual.size()) {
        out() << "Calibre ran " << expected.size() << " searches, Cousoir " << actual.size() << Qt::endl;
        return 1;
    }
    int differences = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        const QJsonObject a = expected.at(i).toObject();
        const QJsonObject b = actual.at(i).toObject();
        const QString query = a.value(QLatin1String("query")).toString();
        if (a.contains(QLatin1String("crash"))) {
            out() << "  " << query << ": not compared, Calibre fails with "
                  << a.value(QLatin1String("crash")).toString() << Qt::endl;
            continue;
        }
        const bool failedA = a.value(QLatin1String("error")).toBool();
        const bool failedB = b.value(QLatin1String("error")).toBool();
        if (failedA != failedB) {
            ++differences;
            out() << "  " << query << ": " << (failedA ? "Calibre fails, Cousoir finds " : "Cousoir fails, Calibre finds ")
                  << display(failedA ? b.value(QLatin1String("ids")) : a.value(QLatin1String("ids"))).left(200)
                  << Qt::endl;
            continue;
        }
        const QJsonArray idsA = a.value(QLatin1String("ids")).toArray();
        const QJsonArray idsB = b.value(QLatin1String("ids")).toArray();
        if (idsA == idsB)
            continue;
        ++differences;
        QSet<int> setA;
        QSet<int> setB;
        for (const QJsonValue &id : idsA)
            setA.insert(id.toInt());
        for (const QJsonValue &id : idsB)
            setB.insert(id.toInt());
        QList<int> onlyA = (setA - setB).values();
        QList<int> onlyB = (setB - setA).values();
        std::sort(onlyA.begin(), onlyA.end());
        std::sort(onlyB.begin(), onlyB.end());
        const auto list = [](const QList<int> &ids) {
            QStringList texts;
            for (const int id : ids.mid(0, 12))
                texts.append(QString::number(id));
            return texts.join(u',') + (ids.size() > 12 ? QStringLiteral("… (%1)").arg(ids.size()) : QString());
        };
        out() << "  " << query << ": " << idsA.size() << " books for Calibre, " << idsB.size() << " for Cousoir";
        if (!onlyA.isEmpty())
            out() << "; only Calibre's: " << list(onlyA);
        if (!onlyB.isEmpty())
            out() << "; only Cousoir's: " << list(onlyB);
        out() << Qt::endl;
    }
    out() << expected.size() - differences << " of " << expected.size() << " searches alike" << Qt::endl;
    return differences ? 1 : 0;
}

} // namespace Tool
