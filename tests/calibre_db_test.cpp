#include <QtTest>

#include <QCryptographicHash>
#include <QDirIterator>
#include <QTemporaryDir>

#include "calibre_db.h"
#include "calibre_list.h"
#include "sqlite_db.h"
#include "test_support.h"

namespace {

// Every file under `folder` with its size, time and hash: enough to see that
// nothing in it changed.
QStringList tree(const QString &folder)
{
    QStringList entries;
    QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        entries.append(QStringLiteral("%1 %2 %3 %4").arg(QDir(folder).relativeFilePath(info.filePath()))
                           .arg(info.size())
                           .arg(info.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch())
                           .arg(info.isFile() ? QString::fromLatin1(QCryptographicHash::hash(
                                    readAll(info.filePath()), QCryptographicHash::Sha256).toHex()) : QString()));
    }
    entries.sort();
    return entries;
}

CalibreConfig frenchConfig()
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    return config;
}

} // namespace

class CalibreDbTest : public QObject
{
    Q_OBJECT

private slots:
    void listsLikeCalibredb();
    void readsBooks();
    void readsBrowseItems();
    void leavesLibraryUntouched();
    void preparesConnectionLikeCalibre();
    void knowsSchema();
    void flagsUnknownSchema();
    void refusesWhileCalibreWrites();
    void reportsMissingLibrary();
    void findsRenamedAndMissingFiles();
    void parsesDates_data();
    void parsesDates();

private:
    bool load(LibraryData *data, const QString &library = fixtureLibrary());

    QTemporaryDir m_cache;
};

bool CalibreDbTest::load(LibraryData *data, const QString &library)
{
    QString error;
    const bool ok = CalibreDb::loadLibrary(library, m_cache.filePath(QStringLiteral("snapshot.db")), frenchConfig(),
                                           data, &error);
    if (!ok)
        qWarning() << error;
    return ok;
}

void CalibreDbTest::listsLikeCalibredb()
{
    // tools/make_fixture_library.sh saved calibredb's output with the
    // library's folder as @LIBRARY@.
    LibraryData data;
    QVERIFY(load(&data));
    QByteArray expected = readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library-list.json"));
    QVERIFY(!expected.isEmpty());
    expected.replace("@LIBRARY@", QDir::cleanPath(QFileInfo(fixtureLibrary()).absoluteFilePath()).toUtf8());
    const QByteArray actual = CalibreList::machineList(data);
    if (actual != expected) {
        const QList<QByteArray> actualLines = actual.split('\n');
        const QList<QByteArray> expectedLines = expected.split('\n');
        for (qsizetype line = 0; line < std::max(actualLines.size(), expectedLines.size()); ++line)
            QCOMPARE(actualLines.value(line), expectedLines.value(line));
    }
    QCOMPARE(actual, expected);
}

void CalibreDbTest::readsBooks()
{
    LibraryData data;
    QVERIFY(load(&data));
    QCOMPARE(data.books.size(), 6);
    QCOMPARE(data.userVersion, CalibreDb::SchemaVersion);
    QVERIFY(!data.uuid.isEmpty());

    const Book &ecume = data.books.at(0);
    QCOMPARE(ecume.title, QStringLiteral("L'Écume des jours"));
    // Calibre sorted it with French articles, the interface's.
    QCOMPARE(ecume.sort, QStringLiteral("Écume des jours, L'"));
    QCOMPARE(ecume.authors, QStringList{ QStringLiteral("Boris Vian") });
    QCOMPARE(ecume.authorSort, QStringLiteral("Vian, Boris"));
    QCOMPARE(ecume.tags, (QStringList{ QStringLiteral("Roman"), QStringLiteral("Classique") }));
    QCOMPARE(ecume.publisher, QStringLiteral("Gallimard"));
    QCOMPARE(ecume.rating, 8);
    QCOMPARE(ecume.languages, QStringList{ QStringLiteral("fra") });
    QCOMPARE(ecume.identifiers.size(), 1);
    QCOMPARE(ecume.identifiers.first().first, QStringLiteral("isbn"));
    QCOMPARE(ecume.pubdate, QDateTime(QDate(1947, 3, 20), QTime(0, 0), QTimeZone::UTC));
    QVERIFY(ecume.comments.contains(QStringLiteral("<em>1946</em>")));
    QCOMPARE(ecume.formats.size(), 1);
    QVERIFY(QFileInfo(ecume.formats.first().file).isFile());
    QVERIFY(QFileInfo(ecume.coverFile).isFile());
    QVERIFY(ecume.coverModified > 0);

    const Book &black = data.books.at(1);
    QCOMPARE(black.series, QStringLiteral("Chronicles of the Black Company"));
    QCOMPARE(black.seriesIndex, 1.0);
    QCOMPARE(black.formats.size(), 2);
    QCOMPARE(black.formats.at(0).format, QStringLiteral("AZW3"));
    QCOMPARE(black.formats.at(1).format, QStringLiteral("EPUB"));

    const Book &omens = data.books.at(3);
    QCOMPARE(omens.authors, (QStringList{ QStringLiteral("Terry Pratchett"), QStringLiteral("Neil Gaiman") }));
    QCOMPARE(omens.authorSort, QStringLiteral("Pratchett, Terry & Gaiman, Neil"));
    QCOMPARE(omens.rating, 7);

    const Book &manual = data.books.at(4);
    QVERIFY(manual.coverFile.isEmpty());
    QVERIFY(!manual.hasCover);
    QCOMPARE(manual.formats.first().format, QStringLiteral("PDF"));

    const Book &prince = data.books.at(5);
    QCOMPARE(prince.authorSort, QStringLiteral("Saint-Exupéry, Antoine de"));
    QCOMPARE(prince.seriesIndex, 1.5);
    QVERIFY(prince.formats.isEmpty());
    QVERIFY(CalibreDb::isUndefinedDate(prince.pubdate));
}

void CalibreDbTest::readsBrowseItems()
{
    LibraryData data;
    QVERIFY(load(&data));
    QCOMPARE(data.authors.size(), 6);
    const auto glen = std::find_if(data.authors.cbegin(), data.authors.cend(),
                                   [](const FacetItem &item) { return item.name == QLatin1String("Glen Cook"); });
    QVERIFY(glen != data.authors.cend());
    QCOMPARE(glen->sort, QStringLiteral("Cook, Glen"));
    QCOMPARE(glen->count, 2);
    QCOMPARE(data.series.size(), 2);
    QCOMPARE(data.tags.size(), 5);
    QCOMPARE(data.publishers.size(), 3);
    QCOMPARE(data.languages.size(), 2);
    for (const FacetItem &language : data.languages) {
        if (language.sort == QLatin1String("fra")) {
            QCOMPARE(language.name, QStringLiteral("French"));
            QCOMPARE(language.count, 3);
        }
    }
}

void CalibreDbTest::leavesLibraryUntouched()
{
    // Reading copies metadata.db out: nothing beside it appears or changes,
    // no journal and no new folder.
    const QStringList before = tree(fixtureLibrary());
    LibraryData data;
    QVERIFY(load(&data));
    QCOMPARE(tree(fixtureLibrary()), before);
}

void CalibreDbTest::preparesConnectionLikeCalibre()
{
    QString error;
    const QString snapshot = m_cache.filePath(QStringLiteral("functions.db"));
    QVERIFY(CalibreDb::takeSnapshot(fixtureLibrary(), snapshot, nullptr, &error));
    SqliteDb db;
    QVERIFY2(db.open(snapshot, SqliteDb::Mode::Immutable, &error), qPrintable(error));
    QVERIFY2(CalibreDb::prepareConnection(db, CalibreText::Tweaks::defaults(), QStringLiteral("fr"), &error),
             qPrintable(error));

    QCOMPARE(db.value(QStringLiteral("SELECT title_sort('Le Petit Prince')")).toString(),
             QStringLiteral("Petit Prince, Le"));
    QCOMPARE(db.value(QStringLiteral("SELECT title_sort('Le Petit Prince', 'strictly_alphabetic')")).toString(),
             QStringLiteral("Le Petit Prince"));
    QCOMPARE(db.value(QStringLiteral("SELECT title_sort('The Hobbit', NULL, 'eng')")).toString(),
             QStringLiteral("Hobbit, The"));
    QCOMPARE(db.value(QStringLiteral("SELECT author_to_author_sort('Doe| John')")).toString(),
             QStringLiteral("Doe, John"));
    QCOMPARE(db.value(QStringLiteral("SELECT length(uuid4())")).toInt(), 36);
    QCOMPARE(db.value(QStringLiteral("SELECT books_list_filter(3)")).toInt(), 1);
    QCOMPARE(db.value(QStringLiteral("SELECT concat(name) FROM (SELECT name FROM tags ORDER BY name)")).toString(),
             QStringLiteral("Classique,Fantasy,Humour,Roman,Technique"));
    QCOMPARE(db.value(QStringLiteral("SELECT sortconcat_amper(link.id, authors.name) FROM books_authors_link link "
                                     "JOIN authors ON authors.id = link.author WHERE link.book = 4")).toString(),
             QStringLiteral("Terry Pratchett&Neil Gaiman"));
    QCOMPARE(db.value(QStringLiteral("SELECT identifiers_concat(type, val) FROM identifiers WHERE book = 4 "
                                     "ORDER BY type")).toString(),
             QStringLiteral("goodreads:12067,isbn:9780060853983"));
    // Calibre's tag browser views call the functions.
    QCOMPARE(db.value(QStringLiteral("SELECT count FROM tag_browser_filtered_series WHERE name = 'Contes'")).toInt(), 1);
    QCOMPARE(db.value(QStringLiteral("SELECT count(*) FROM meta")).toInt(), 6);
    QCOMPARE(db.value(QStringLiteral("SELECT count(*) FROM sqlite_temp_master WHERE type = 'trigger'")).toInt(), 2);
}

void CalibreDbTest::knowsSchema()
{
    LibraryData data;
    QVERIFY(load(&data));
    QVERIFY(data.schemaKnown);
    QVERIFY(CalibreDb::isKnownSchema(CalibreDb::SchemaVersion, data.schemaFingerprint));
    QVERIFY(!CalibreDb::isKnownSchema(29, data.schemaFingerprint));
}

void CalibreDbTest::flagsUnknownSchema()
{
    // A column Calibre added in a later version changes the fingerprint.
    QTemporaryDir library;
    QVERIFY(copyTree(fixtureLibrary(), library.path()));
    {
        SqliteDb db;
        QString error;
        QVERIFY(db.open(library.filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY2(db.exec(QStringLiteral("ALTER TABLE books ADD COLUMN reading_time INTEGER"), &error), qPrintable(error));
    }
    LibraryData data;
    QVERIFY(load(&data, library.path()));
    QVERIFY(!data.schemaKnown);
    QCOMPARE(data.books.size(), 6);

    // A custom column does not.
    QVERIFY(copyTree(fixtureLibrary(), library.filePath(QStringLiteral("custom"))));
    {
        SqliteDb db;
        QString error;
        QVERIFY(db.open(library.filePath(QStringLiteral("custom/metadata.db")), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY2(db.exec(QStringLiteral(
            "INSERT INTO custom_columns (label, name, datatype, normalized) VALUES ('read', 'Read', 'bool', 0);"
            "CREATE TABLE custom_column_1 (id INTEGER PRIMARY KEY, book INTEGER, value BOOL, UNIQUE(book));"), &error),
            qPrintable(error));
    }
    QVERIFY(load(&data, library.filePath(QStringLiteral("custom"))));
    QVERIFY(data.schemaKnown);
}

void CalibreDbTest::refusesWhileCalibreWrites()
{
    QTemporaryDir library;
    QVERIFY(copyTree(fixtureLibrary(), library.path()));
    QFile journal(library.filePath(QStringLiteral("metadata.db-journal")));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.close();
    LibraryData data;
    QString error;
    QVERIFY(!CalibreDb::loadLibrary(library.path(), m_cache.filePath(QStringLiteral("busy.db")), frenchConfig(),
                                    &data, &error));
    QVERIFY2(error.contains(QStringLiteral("Calibre is writing")), qPrintable(error));
}

void CalibreDbTest::reportsMissingLibrary()
{
    QTemporaryDir empty;
    LibraryData data;
    QString error;
    QVERIFY(!CalibreDb::loadLibrary(empty.path(), m_cache.filePath(QStringLiteral("none.db")), frenchConfig(),
                                    &data, &error));
    QVERIFY2(error.contains(QStringLiteral("no Calibre library")), qPrintable(error));
}

void CalibreDbTest::findsRenamedAndMissingFiles()
{
    QTemporaryDir library;
    QVERIFY(copyTree(fixtureLibrary(), library.path()));
    const QDir black(library.filePath(QStringLiteral("Glen Cook/The Black Company (2)")));
    // calibredb list finds a format under another name by its extension.
    QVERIFY(QFile::rename(black.filePath(QStringLiteral("The Black Company - Glen Cook.epub")),
                          black.filePath(QStringLiteral("renamed.epub"))));
    QVERIFY(QFile::remove(black.filePath(QStringLiteral("The Black Company - Glen Cook.azw3"))));
    QVERIFY(QFile::remove(black.filePath(QStringLiteral("cover.jpg"))));
    LibraryData data;
    QVERIFY(load(&data, library.path()));
    const Book &book = data.books.at(1);
    QCOMPARE(book.formats.at(0).file, QString());
    QCOMPARE(book.formats.at(1).file, black.filePath(QStringLiteral("renamed.epub")));
    QVERIFY(book.coverFile.isEmpty());
    QVERIFY(book.hasCover);
}

void CalibreDbTest::parsesDates_data()
{
    QTest::addColumn<QVariant>("stored");
    QTest::addColumn<QString>("iso");
    QTest::newRow("microseconds") << QVariant(QStringLiteral("2015-07-07 18:41:43.605980+00:00"))
                                  << QStringLiteral("2015-07-07T18:41:43+00:00");
    QTest::newRow("seconds") << QVariant(QStringLiteral("2016-02-06 20:16:57+00:00"))
                             << QStringLiteral("2016-02-06T20:16:57+00:00");
    QTest::newRow("offset") << QVariant(QStringLiteral("2016-02-06T22:16:57+02:00"))
                            << QStringLiteral("2016-02-06T20:16:57+00:00");
    QTest::newRow("undefined") << QVariant(QStringLiteral("0101-01-01 00:00:00+00:00"))
                               << QStringLiteral("0101-01-01T00:00:00+00:00");
    QTest::newRow("null") << QVariant() << QStringLiteral("0101-01-01T00:00:00+00:00");
    QTest::newRow("year") << QVariant(qint64(2001)) << QStringLiteral("2001-01-03T00:00:00+00:00");
    QTest::newRow("garbage") << QVariant(QStringLiteral("soon")) << QStringLiteral("0101-01-01T00:00:00+00:00");
}

void CalibreDbTest::parsesDates()
{
    QFETCH(QVariant, stored);
    QFETCH(QString, iso);
    QCOMPARE(CalibreDb::isoFormat(CalibreDb::parseDate(stored)), iso);
}

QTEST_GUILESS_MAIN(CalibreDbTest)
#include "calibre_db_test.moc"
