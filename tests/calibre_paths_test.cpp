#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "calibre_paths.h"
#include "calibre_text.h"

namespace {

// What Calibre 9.16 makes of the same names, written by
// tools/oracle/paths_oracle.py.
QJsonObject oracle()
{
    static const QJsonObject data = [] {
        QFile file(QStringLiteral(RELIURE_TEST_DATA_DIR "/oracle/paths.json"));
        if (!file.open(QIODevice::ReadOnly))
            return QJsonObject();
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return data;
}

} // namespace

class CalibrePathsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void asciiFilename_data();
    void asciiFilename();
    void paths_data();
    void paths();
    void translatesCalibreStrings();
};

void CalibrePathsTest::initTestCase()
{
    QVERIFY2(!oracle().isEmpty(), "tests/data/oracle/paths.json is missing; run make oracle");
    QCOMPARE(oracle().value(QLatin1String("interface_language")).toString(), QStringLiteral("fr"));
}

void CalibrePathsTest::asciiFilename_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");
    int row = 0;
    for (const QJsonValue &value : oracle().value(QLatin1String("ascii_filename")).toArray()) {
        const QJsonObject test = value.toObject();
        QTest::addRow("%d", row++) << test.value(QLatin1String("text")).toString()
                                   << test.value(QLatin1String("expected")).toString();
    }
}

void CalibrePathsTest::asciiFilename()
{
    QFETCH(QString, text);
    QFETCH(QString, expected);
    QCOMPARE(CalibreText::asciiFilename(text), expected);
}

void CalibrePathsTest::paths_data()
{
    QTest::addColumn<int>("bookId");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("author");
    QTest::addColumn<int>("extensionLength");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("file");
    int row = 0;
    for (const QJsonValue &value : oracle().value(QLatin1String("paths")).toArray()) {
        const QJsonObject test = value.toObject();
        QTest::addRow("%d", row++) << test.value(QLatin1String("id")).toInt()
                                   << test.value(QLatin1String("title")).toString()
                                   << test.value(QLatin1String("author")).toString()
                                   << test.value(QLatin1String("extlen")).toInt()
                                   << test.value(QLatin1String("path")).toString()
                                   << test.value(QLatin1String("file")).toString();
    }
}

void CalibrePathsTest::paths()
{
    QFETCH(int, bookId);
    QFETCH(QString, title);
    QFETCH(QString, author);
    QFETCH(int, extensionLength);
    QFETCH(QString, path);
    QFETCH(QString, file);
    const QString unknown = CalibreText::unknownText(QStringLiteral("fr"));
    QCOMPARE(CalibrePaths::constructPathName(bookId, title, author, unknown), path);
    QCOMPARE(CalibrePaths::constructFileName(bookId, title, author, extensionLength, unknown), file);
}

void CalibrePathsTest::translatesCalibreStrings()
{
    QCOMPARE(CalibreText::unknownText(QStringLiteral("fr")), QStringLiteral("Inconnu(e)"));
    QCOMPARE(CalibreText::coverText(QStringLiteral("fr_FR.UTF-8")), QStringLiteral("Couverture"));
    QCOMPARE(CalibreText::unknownText(QStringLiteral("pt_BR")), QStringLiteral("Desconhecido"));
    QCOMPARE(CalibreText::unknownText(QStringLiteral("en_GB")), QStringLiteral("Unknown"));
    QCOMPARE(CalibreText::coverText(QStringLiteral("xx")), QStringLiteral("Cover"));
    QVERIFY(CalibreText::hasAsciiTable(QStringLiteral("fr")));
    QVERIFY(!CalibreText::hasAsciiTable(QStringLiteral("ja")));
}

QTEST_GUILESS_MAIN(CalibrePathsTest)
#include "calibre_paths_test.moc"
