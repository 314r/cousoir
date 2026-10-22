#include <QtTest>

#include <QTemporaryDir>

#include "calibre_config.h"
#include "calibre_db.h"
#include "calibre_fts.h"
#include "calibre_notes.h"
#include "sqlite_db.h"
#include "test_support.h"

// Notes as Calibre 7 keeps them: read from a notes.db Calibre's own API
// wrote (tools/oracle/notes_fixture.py), and written back as its Notes class
// writes them.
class CalibreNotesTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void hashesAsCalibre();
    void readsNotes();
    void writesNotes();
    void leavesNotesWithImagesToCalibre();

private:
    std::unique_ptr<QTemporaryDir> m_folder;
    QString m_library;
};

void CalibreNotesTest::init()
{
    m_folder = std::make_unique<QTemporaryDir>();
    m_library = m_folder->filePath(QStringLiteral("library"));
    QVERIFY(copyTree(fixtureLibrary(), m_library));
    QDir(CalibreDb::notesFolder(m_library)).removeRecursively();
    QVERIFY(copyTree(QStringLiteral(COUSOIR_TEST_DATA_DIR "/notes/.calnotes"), CalibreDb::notesFolder(m_library)));
}

void CalibreNotesTest::hashesAsCalibre()
{
    // hash_key() of Calibre, for the folders of the notes kept aside.
    QCOMPARE(CalibreNotes::hashKey(QStringLiteral("authors glen cook")), QStringLiteral("a95cb7309f97cdf2"));
    QCOMPARE(CalibreNotes::hashKey(QStringLiteral("tags fantasy")), QStringLiteral("818dc9926f3636e4"));
}

void CalibreNotesTest::readsNotes()
{
    LibraryData data;
    data.libraryPath = m_library;
    CalibreDb::readNotes(&data);
    QCOMPARE(data.notes.value(QStringLiteral("authors")).size(), 1);
    const ItemNote author = data.notes.value(QStringLiteral("authors")).value(2);
    QVERIFY(author.doc.contains(QStringLiteral("<b>dark fantasy</b>")));
    QCOMPARE(author.searchable, QStringLiteral("Glen Cook\nAn American writer of dark fantasy."));
    QCOMPARE(author.resources, QStringList{ QStringLiteral("xxh64:5f8c87fa2ca5d829") });
    QVERIFY(author.created > 0);
    QCOMPARE(data.notes.value(QStringLiteral("series")).value(2).searchable,
             QStringLiteral("Contes\nTales for children, and others."));
    QVERIFY(data.notes.value(QStringLiteral("tags")).contains(3));
    QCOMPARE(CalibreDb::notesField(QStringLiteral("publishers")), QStringLiteral("publisher"));
}

void CalibreNotesTest::writesNotes()
{
    QString error;
    const QList<CalibreNotes::Change> changes{
        { QStringLiteral("tags"), 3, QStringLiteral("Fantasy"), QStringLiteral("<p>Other worlds, élégantes.</p>"),
          QStringLiteral("Other worlds, élégantes.") },
        { QStringLiteral("publisher"), 2, QStringLiteral("Tor"), QStringLiteral("<p>New York.</p>"), QStringLiteral("New York.") },
        { QStringLiteral("series"), 2, QStringLiteral("Contes"), QString(), QString() },
    };
    QVERIFY2(CalibreNotes::apply(m_library, changes, QStringLiteral("fr"), &error), qPrintable(error));

    const QDir notes(CalibreDb::notesFolder(m_library));
    SqliteDb db;
    QVERIFY2(db.open(notes.filePath(QStringLiteral("notes.db")), SqliteDb::Mode::ReadWrite, &error), qPrintable(error));
    QVERIFY(CalibreFts::registerTokenizers(db.handle(), QStringLiteral("fr"), &error));
    QCOMPARE(db.value(QStringLiteral("SELECT searchable_text FROM notes WHERE colname='tags' AND item=3")).toString(),
             QStringLiteral("Fantasy\nOther worlds, élégantes."));
    QCOMPARE(db.value(QStringLiteral("SELECT doc FROM notes WHERE colname='publisher' AND item=2")).toString(),
             QStringLiteral("<p>New York.</p>"));
    QVERIFY(!db.value(QStringLiteral("SELECT id FROM notes WHERE colname='series'")).isValid());
    // The index agrees with the notes, found by a word, accents aside.
    QVERIFY2(db.exec(QStringLiteral("INSERT INTO notes_fts(notes_fts) VALUES('integrity-check')"), &error), qPrintable(error));
    QCOMPARE(db.value(QStringLiteral("SELECT count(*) FROM notes_fts WHERE notes_fts MATCH 'elegantes'")).toInt(), 1);
    db.close();

    // The backup beside, as Calibre writes one.
    QCOMPARE(readAll(notes.filePath(QStringLiteral("backup/publisher/2"))),
             QByteArray("<p>New York.</p>") + QByteArray("\0\x1c\0", 3) + "Tor\nNew York." + QByteArray("\0\x1c\0", 3)
                 + QByteArray("\0\x1c\0", 3) + "Tor");
    // The series' note kept aside, where Calibre looks to bring it back.
    const QString retired = notes.filePath(QStringLiteral("retired/") + CalibreNotes::hashKey(QStringLiteral("series contes")));
    QVERIFY(readAll(QDir(retired).filePath(QStringLiteral("doc.html"))).startsWith("<p>Tales for children"));
    QCOMPARE(readAll(QDir(retired).filePath(QStringLiteral("note_id"))), QByteArray("2"));
    QVERIFY(!QFileInfo::exists(notes.filePath(QStringLiteral("backup/series/2"))));
}

void CalibreNotesTest::leavesNotesWithImagesToCalibre()
{
    QString error;
    QVERIFY(!CalibreNotes::apply(m_library,
                                 { { QStringLiteral("authors"), 2, QStringLiteral("Glen Cook"), QStringLiteral("<p>x</p>"),
                                     QStringLiteral("x") } },
                                 QStringLiteral("fr"), &error));
    QVERIFY(!error.isEmpty());
    // A library Calibre 7 never opened has no notes to write.
    QDir(CalibreDb::notesFolder(m_library)).removeRecursively();
    QVERIFY(!CalibreNotes::apply(m_library,
                                 { { QStringLiteral("tags"), 3, QStringLiteral("Fantasy"), QStringLiteral("<p>x</p>"),
                                     QStringLiteral("x") } },
                                 QStringLiteral("fr"), &error));
}

QTEST_GUILESS_MAIN(CalibreNotesTest)

#include "calibre_notes_test.moc"
