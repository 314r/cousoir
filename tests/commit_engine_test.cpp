#include <QtTest>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QImage>
#include <QTemporaryDir>

#include "calibre_db.h"
#include "calibre_lock.h"
#include "commit_engine.h"
#include "field_writers.h"
#include "file_transaction.h"
#include "sqlite_db.h"

namespace {

QString fixtureLibrary()
{
    return QStringLiteral(RELIURE_TEST_DATA_DIR "/library");
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool copyTree(const QString &from, const QString &to)
{
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString target = QDir(to).filePath(QDir(from).relativeFilePath(source));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(source, target))
            return false;
    }
    return true;
}

// Every file under `folder` with its hash.
QStringList tree(const QString &folder)
{
    QStringList entries;
    QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        entries.append(QDir(folder).relativeFilePath(info.filePath()) + u' '
                       + (info.isFile() ? QString::fromLatin1(QCryptographicHash::hash(readAll(info.filePath()),
                                                                                      QCryptographicHash::Sha256).toHex())
                                        : QStringLiteral("dir")));
    }
    entries.sort();
    return entries;
}

QByteArray image(int width, int height, const char *format, bool transparent = false)
{
    QImage picture(width, height, transparent ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    picture.fill(transparent ? QColor(20, 40, 200, 100) : QColor(200, 40, 20));
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    picture.save(&buffer, format);
    return data;
}

BookEdit edit(int book, const QVariantMap &changes)
{
    BookEdit result;
    QString error;
    if (!BookEdit::fromVariantMap(book, changes, &result, &error))
        qWarning() << error;
    return result;
}

} // namespace

class CommitEngineTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void renamesBookFolder();
    void followsAuthorCase();
    void removesUnusedItemsButNoted();
    void cleansValues();
    void parsesDates_data();
    void parsesDates();
    void writesCovers();
    void changesNothingForSameValues();
    void refusesWhileCalibreRuns();
    void refusesUnknownTransliteration();
    void refusesWithJournal();
    void refusesUnknownSchema();
    void refusesToReplaceFolder();
    void undoesFailedChange();
    void rebasesOnConcurrentChange();
    void recoversInterruptedChange();
    void finishesPublishedChange();

private:
    CommitEngine::Result commit(const QList<BookEdit> &edits);
    QVariant value(const QString &sql);
    QString book(int id, const QString &column);

    std::unique_ptr<QTemporaryDir> m_root;
    QString m_library;
    QString m_cache;
    QString m_backups;
    QDateTime m_now;
};

void CommitEngineTest::init()
{
    m_root = std::make_unique<QTemporaryDir>();
    m_library = m_root->filePath(QStringLiteral("library"));
    m_cache = m_root->filePath(QStringLiteral("cache"));
    m_backups = m_root->filePath(QStringLiteral("backups"));
    QVERIFY(copyTree(fixtureLibrary(), m_library));
    // Older than any write.
    m_now = QDateTime(QDate(2026, 10, 19), QTime(9, 30, 15, 250), QTimeZone::UTC);
}

CommitEngine::Result CommitEngineTest::commit(const QList<BookEdit> &edits)
{
    CommitEngine::Request request;
    request.libraryPath = m_library;
    request.cacheFolder = m_cache;
    request.backupFolder = m_backups;
    request.config.interfaceLanguage = QStringLiteral("fr");
    request.edits = edits;
    request.producer = QStringLiteral("Reliure (test)");
    request.producerId = QStringLiteral("reliure");
    request.now = m_now;
    return CommitEngine::commit(request);
}

QVariant CommitEngineTest::value(const QString &sql)
{
    SqliteDb db;
    QString error;
    if (!db.open(QDir(m_library).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::Immutable, &error))
        return error;
    CalibreDb::prepareConnection(db, CalibreText::Tweaks::defaults(), QStringLiteral("fr"), &error);
    return db.value(sql);
}

QString CommitEngineTest::book(int id, const QString &column)
{
    return value(QStringLiteral("SELECT %1 FROM books WHERE id=%2").arg(column).arg(id)).toString();
}

void CommitEngineTest::renamesBookFolder()
{
    const CommitEngine::Result result = commit({ edit(1, { { QStringLiteral("title"),
                                                             QStringLiteral("L'Écume des jours (illustrée)") } }) });
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.dirtied, QSet<int>{ 1 });
    QCOMPARE(book(1, QStringLiteral("path")), QStringLiteral("Boris Vian/L'Ecume des jours (illustree) (1)"));
    QCOMPARE(book(1, QStringLiteral("sort")), QStringLiteral("Écume des jours (illustrée), L'"));
    QCOMPARE(book(1, QStringLiteral("last_modified")), QStringLiteral("2026-10-19 09:30:15.250000+00:00"));
    QCOMPARE(value(QStringLiteral("SELECT name FROM data WHERE book=1")).toString(),
             QStringLiteral("L'Ecume des jours (illustree) - Boris Vian"));
    QCOMPARE(value(QStringLiteral("SELECT group_concat(book) FROM metadata_dirtied")).toString(), QStringLiteral("1"));

    const QDir folder(QDir(m_library).filePath(QStringLiteral("Boris Vian/L'Ecume des jours (illustree) (1)")));
    QVERIFY(QFileInfo(folder.filePath(QStringLiteral("L'Ecume des jours (illustree) - Boris Vian.epub"))).isFile());
    QVERIFY(QFileInfo(folder.filePath(QStringLiteral("cover.jpg"))).isFile());
    QVERIFY(readAll(folder.filePath(QStringLiteral("metadata.opf"))).contains("<dc:title>L'Écume des jours (illustrée)</dc:title>"));
    QVERIFY(!QFileInfo::exists(QDir(m_library).filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)"))));
    // Nothing of the change is left behind, and a backup was made.
    QVERIFY(!tree(m_library).join(u'\n').contains(QStringLiteral(".~reliure")));
    QVERIFY(!QFileInfo::exists(QDir(m_cache).filePath(QStringLiteral("ops.journal"))));
    QVERIFY(QFileInfo::exists(result.backup));
    QCOMPARE(readAll(result.backup), readAll(QDir(fixtureLibrary()).filePath(QStringLiteral("metadata.db"))));
    // What the application shows next comes with the result.
    QCOMPARE(result.data.books.first().title, QStringLiteral("L'Écume des jours (illustrée)"));
    QVERIFY(!result.data.books.first().formats.first().file.isEmpty());
    QCOMPARE(result.data.source, CalibreDb::identify(QDir(m_library).filePath(QStringLiteral("metadata.db"))));
}

void CommitEngineTest::followsAuthorCase()
{
    const CommitEngine::Result result = commit({ edit(3, { { QStringLiteral("authors"),
                                                             QStringList{ QStringLiteral("glen cook") } } }) });
    QVERIFY2(result.ok, qPrintable(result.error));
    // The author is renamed for every book of his, which all move.
    QCOMPARE(result.dirtied, (QSet<int>{ 2, 3 }));
    QCOMPARE(book(2, QStringLiteral("path")), QStringLiteral("glen cook/The Black Company (2)"));
    QCOMPARE(book(3, QStringLiteral("author_sort")), QStringLiteral("cook, glen"));
    QCOMPARE(value(QStringLiteral("SELECT count(*) FROM authors WHERE name='glen cook'")).toInt(), 1);
    QVERIFY(!QFileInfo::exists(QDir(m_library).filePath(QStringLiteral("Glen Cook"))));
    QVERIFY(QFileInfo(QDir(m_library).filePath(QStringLiteral("glen cook/The Black Company (2)/The Black Company - glen cook.azw3"))).isFile());
}

void CommitEngineTest::removesUnusedItemsButNoted()
{
    // A note on the publisher Tor keeps it when no book has it any more.
    QVERIFY(QDir().mkpath(QDir(m_library).filePath(QStringLiteral(".calnotes"))));
    {
        SqliteDb notes;
        QString error;
        QVERIFY(notes.open(QDir(m_library).filePath(QStringLiteral(".calnotes/notes.db")), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY(notes.exec(QStringLiteral("DROP TABLE IF EXISTS notes; CREATE TABLE notes (id INTEGER PRIMARY KEY, item INTEGER, colname TEXT);"
                                          "INSERT INTO notes (item, colname) VALUES ((SELECT 2), 'publisher')"), &error));
    }
    const CommitEngine::Result result = commit({
        edit(1, { { QStringLiteral("tags"), QStringList{ QStringLiteral("Roman") } } }),
        edit(2, { { QStringLiteral("publisher"), QString() } }),
    });
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(value(QStringLiteral("SELECT count(*) FROM tags WHERE name='Classique'")).toInt(), 0);
    QCOMPARE(value(QStringLiteral("SELECT count(*) FROM books_publishers_link WHERE book=2")).toInt(), 0);
    QCOMPARE(value(QStringLiteral("SELECT name FROM publishers WHERE id=2")).toString(), QStringLiteral("Tor"));
}

void CommitEngineTest::cleansValues()
{
    const CommitEngine::Result result = commit({ edit(4, {
        { QStringLiteral("title"), QStringLiteral("  Good   Omens ") },
        { QStringLiteral("tags"), QStringLiteral("humour,  Fantasy , new  tag, NEW TAG") },
        { QStringLiteral("languages"), QStringLiteral("French, en, xx, und") },
        { QStringLiteral("identifiers"), QStringLiteral("ISBN: 978-0, Goodreads:1,2, empty: ") },
        { QStringLiteral("series"), QStringLiteral("Discworld Extras [3.5]") },
        { QStringLiteral("rating"), 7 },
        { QStringLiteral("comments"), QStringLiteral("<p>One\n\n   two</p>") },
        { QStringLiteral("authors"), QStringLiteral("Terry  Pratchett & Neil Gaiman & A|B") },
    }) });
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(book(4, QStringLiteral("title")), QStringLiteral("Good Omens"));
    QCOMPARE(value(QStringLiteral("SELECT group_concat(tags.name, '|' ORDER BY l.id) FROM books_tags_link l "
                                  "JOIN tags ON tags.id=l.tag WHERE l.book=4")).toString(),
             QStringLiteral("humour|Fantasy|new tag"));
    // A case change renames the tag for every book.
    QCOMPARE(value(QStringLiteral("SELECT name FROM tags WHERE lower(name)='humour'")).toString(), QStringLiteral("humour"));
    QCOMPARE(value(QStringLiteral("SELECT group_concat(languages.lang_code, ',' ORDER BY l.id) FROM books_languages_link l "
                                  "JOIN languages ON languages.id=l.lang_code WHERE l.book=4")).toString(),
             QStringLiteral("fra,eng"));
    // Text splits at commas, as Calibre's identifiers box does.
    QCOMPARE(value(QStringLiteral("SELECT group_concat(type || '=' || val, ' ' ORDER BY id) FROM identifiers "
                                  "WHERE book=4")).toString(),
             QStringLiteral("isbn=978-0 goodreads=1"));
    QCOMPARE(value(QStringLiteral("SELECT series.name FROM books_series_link l JOIN series ON series.id=l.series "
                                  "WHERE l.book=4")).toString(), QStringLiteral("Discworld Extras"));
    QCOMPARE(book(4, QStringLiteral("series_index")).toDouble(), 3.5);
    QCOMPARE(value(QStringLiteral("SELECT ratings.rating FROM books_ratings_link l JOIN ratings ON ratings.id=l.rating "
                                  "WHERE l.book=4")).toInt(), 7);
    QCOMPARE(value(QStringLiteral("SELECT text FROM comments WHERE book=4")).toString(), QStringLiteral("<p>One two</p>"));
    // A "|" in a name is a comma; Calibre stores a comma as "|".
    QCOMPARE(value(QStringLiteral("SELECT name FROM authors WHERE name LIKE 'A%B'")).toString(), QStringLiteral("A|B"));
    QCOMPARE(book(4, QStringLiteral("author_sort")), QStringLiteral("Pratchett, Terry & Gaiman, Neil & A,B"));
}

void CommitEngineTest::parsesDates_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("stored");
    QTest::newRow("day") << QStringLiteral("1947-03-20") << QStringLiteral("1947-03-20 00:00:00+00:00");
    QTest::newRow("month") << QStringLiteral("1947-03") << QStringLiteral("1947-03-15 00:00:00+00:00");
    QTest::newRow("year") << QStringLiteral("1947") << QStringLiteral("1947-10-15 00:00:00+00:00");
    QTest::newRow("first") << QStringLiteral("1984-05-01") << QStringLiteral("1984-05-02 00:00:00+00:00");
    QTest::newRow("last") << QStringLiteral("1984-04-30") << QStringLiteral("1984-04-29 00:00:00+00:00");
    QTest::newRow("december") << QStringLiteral("1984-12-31") << QStringLiteral("1984-12-31 00:00:00+00:00");
    QTest::newRow("slashes") << QStringLiteral("2001/9/11") << QStringLiteral("2001-09-11 00:00:00+00:00");
    QTest::newRow("no date") << QStringLiteral("soon") << QString();
    QTest::newRow("no day") << QStringLiteral("2001-02-30") << QString();
}

void CommitEngineTest::parsesDates()
{
    QFETCH(QString, text);
    QFETCH(QString, stored);
    const std::optional<QDateTime> date = FieldWriters::parseOnlyDate(text, m_now);
    QCOMPARE(date ? FieldWriters::sqliteDate(*date) : QString(), stored);
}

void CommitEngineTest::writesCovers()
{
    const QByteArray small = image(300, 450, "JPEG");
    // A JPEG that fits is kept byte for byte; a transparent PNG becomes a
    // JPEG on white; a large image is scaled to fit 1650 by 2200.
    QCOMPARE(CommitEngine::coverData(small, nullptr), small);
    const QImage flattened = QImage::fromData(CommitEngine::coverData(image(100, 100, "PNG", true), nullptr), "JPEG");
    QCOMPARE(flattened.size(), QSize(100, 100));
    // 39 % of (20, 40, 200) over white.
    const QRgb pixel = flattened.pixel(50, 50);
    QVERIFY2(qAbs(qRed(pixel) - 163) < 8 && qAbs(qGreen(pixel) - 171) < 8 && qAbs(qBlue(pixel) - 233) < 8,
             qPrintable(QColor(pixel).name()));
    QCOMPARE(QImage::fromData(CommitEngine::coverData(image(3000, 2000, "PNG"), nullptr)).size(), QSize(1650, 1100));
    QString error;
    QVERIFY(CommitEngine::coverData("not an image", &error).isEmpty());
    QVERIFY(!error.isEmpty());

    BookEdit add;
    add.bookId = 5;
    add.cover = small;
    BookEdit removal;
    removal.bookId = 1;
    removal.cover = QByteArray();
    const CommitEngine::Result written = commit({ add, removal });
    QVERIFY2(written.ok, qPrintable(written.error));
    QCOMPARE(book(5, QStringLiteral("has_cover")), QStringLiteral("1"));
    QCOMPARE(readAll(QDir(m_library).filePath(QStringLiteral("Jean-Pierre Dupont/Manuel technique (5)/cover.jpg"))), small);
    QCOMPARE(book(1, QStringLiteral("has_cover")), QStringLiteral("0"));
    QVERIFY(!QFileInfo::exists(QDir(m_library).filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/cover.jpg"))));
    QVERIFY(!tree(m_library).join(u'\n').contains(QStringLiteral(".~reliure")));
}

void CommitEngineTest::changesNothingForSameValues()
{
    const QStringList before = tree(m_library);
    const CommitEngine::Result result = commit({ edit(2, { { QStringLiteral("title"), QStringLiteral("The Black Company") },
                                                           { QStringLiteral("rating"), 10 } }) });
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(result.dirtied.isEmpty());
    QCOMPARE(tree(m_library), before);
}

void CommitEngineTest::refusesWhileCalibreRuns()
{
    CalibreLock calibre;
    QVERIFY(calibre.acquire());
    const QStringList before = tree(m_library);
    const CommitEngine::Result result = commit({ edit(1, { { QStringLiteral("title"), QStringLiteral("X") } }) });
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("Calibre is open")), qPrintable(result.error));
    QCOMPARE(tree(m_library), before);
}

void CommitEngineTest::refusesUnknownTransliteration()
{
    CommitEngine::Request request;
    request.libraryPath = m_library;
    request.cacheFolder = m_cache;
    request.config.interfaceLanguage = QStringLiteral("ja");
    request.edits = { edit(1, { { QStringLiteral("title"), QStringLiteral("X") } }) };
    const CommitEngine::Result result = CommitEngine::commit(request);
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("names folders")), qPrintable(result.error));
}

void CommitEngineTest::refusesWithJournal()
{
    QFile journal(QDir(m_library).filePath(QStringLiteral("metadata.db-journal")));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.close();
    const CommitEngine::Result result = commit({ edit(1, { { QStringLiteral("title"), QStringLiteral("X") } }) });
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("Calibre is writing")), qPrintable(result.error));
}

void CommitEngineTest::refusesUnknownSchema()
{
    {
        SqliteDb db;
        QString error;
        QVERIFY(db.open(QDir(m_library).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY(db.exec(QStringLiteral("ALTER TABLE books ADD COLUMN reading_time INTEGER"), &error));
    }
    const QStringList before = tree(m_library);
    const CommitEngine::Result result = commit({ edit(1, { { QStringLiteral("title"), QStringLiteral("X") } }) });
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("does not know")), qPrintable(result.error));
    QCOMPARE(tree(m_library), before);
}

void CommitEngineTest::refusesToReplaceFolder()
{
    // Calibre would delete what is in the way; Reliure stops.
    const QString stale = QDir(m_library).filePath(QStringLiteral("Boris Vian/Mousse (1)"));
    QVERIFY(QDir().mkpath(stale));
    QFile file(QDir(stale).filePath(QStringLiteral("notes.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    const QStringList before = tree(m_library);
    const CommitEngine::Result result = commit({ edit(1, { { QStringLiteral("title"), QStringLiteral("Mousse") } }) });
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("already has a folder")), qPrintable(result.error));
    QCOMPARE(tree(m_library), before);

    // An empty one is cleared, as Calibre clears it.
    QVERIFY(file.remove());
    const CommitEngine::Result cleared = commit({ edit(1, { { QStringLiteral("title"), QStringLiteral("Mousse") } }) });
    QVERIFY2(cleared.ok, qPrintable(cleared.error));
    QVERIFY(QFileInfo(QDir(stale).filePath(QStringLiteral("Mousse - Boris Vian.epub"))).isFile());
}

void CommitEngineTest::undoesFailedChange()
{
    // The second edit fails after the first moved files: all of it is undone.
    const QStringList before = tree(m_library);
    const CommitEngine::Result result = commit({ edit(1, { { QStringLiteral("title"), QStringLiteral("Autre") } }),
                                                 edit(42, { { QStringLiteral("title"), QStringLiteral("Nowhere") } }) });
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("no longer in the library")), qPrintable(result.error));
    QCOMPARE(tree(m_library), before);
}

void CommitEngineTest::rebasesOnConcurrentChange()
{
    // Dropbox brings another computer's change while the edit is saved: the
    // edit starts again on top of it.
    int calls = 0;
    CommitEngine::Request request;
    request.libraryPath = m_library;
    request.cacheFolder = m_cache;
    request.config.interfaceLanguage = QStringLiteral("fr");
    request.edits = { edit(1, { { QStringLiteral("rating"), 2 } }) };
    request.producer = QStringLiteral("Reliure (test)");
    request.producerId = QStringLiteral("reliure");
    request.beforePublish = [&] {
        if (calls++ > 0)
            return;
        SqliteDb db;
        QString error;
        db.open(QDir(m_library).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error);
        CalibreDb::prepareConnection(db, CalibreText::Tweaks::defaults(), QStringLiteral("fr"), &error);
        db.exec(QStringLiteral("UPDATE books SET series_index=7 WHERE id=6"), &error);
    };
    const CommitEngine::Result result = CommitEngine::commit(request);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(calls, 2);
    QCOMPARE(book(6, QStringLiteral("series_index")).toDouble(), 7.0);
    QCOMPARE(value(QStringLiteral("SELECT ratings.rating FROM books_ratings_link l JOIN ratings ON ratings.id=l.rating "
                                  "WHERE l.book=1")).toInt(), 2);
}

void CommitEngineTest::recoversInterruptedChange()
{
    const QStringList before = tree(m_library);
    const QString journal = QDir(m_cache).filePath(QStringLiteral("ops.journal"));
    const QDir library(m_library);
    {
        FileTransaction files(journal);
        QString error;
        QVERIFY(files.move(library.filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)")),
                           library.filePath(QStringLiteral("Vian/Ecume (1)")), &error));
        QVERIFY(files.write(library.filePath(QStringLiteral("Vian/Ecume (1)/metadata.opf")), "<new/>", &error));
        QVERIFY(files.remove(library.filePath(QStringLiteral("Vian/Ecume (1)/cover.jpg")), &error));
        files.abandon();
    }
    QVERIFY(QFileInfo::exists(journal));
    QVERIFY(tree(m_library) != before);
    QString done;
    QString error;
    QVERIFY2(CommitEngine::recover(m_library, m_cache, &done, &error), qPrintable(error));
    QVERIFY(done.contains(QStringLiteral("undid")));
    QCOMPARE(tree(m_library), before);
    QVERIFY(!QFileInfo::exists(journal));
}

void CommitEngineTest::finishesPublishedChange()
{
    // The database was replaced before the crash: the files stay as they
    // are and what was kept goes.
    const QString journal = QDir(m_cache).filePath(QStringLiteral("ops.journal"));
    const QString opf = QDir(m_library).filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/metadata.opf"));
    {
        FileTransaction files(journal);
        QString error;
        QVERIFY(files.write(opf, "<new/>", &error));
        const QByteArray database = readAll(QDir(m_library).filePath(QStringLiteral("metadata.db")));
        QVERIFY(files.aboutToPublish(QCryptographicHash::hash(database, QCryptographicHash::Sha256), &error));
        files.abandon();
    }
    QString done;
    QString error;
    QVERIFY(CommitEngine::recover(m_library, m_cache, &done, &error));
    QVERIFY(done.contains(QStringLiteral("finished")));
    QCOMPARE(readAll(opf), QByteArray("<new/>"));
    QVERIFY(!tree(m_library).join(u'\n').contains(QStringLiteral(".~reliure")));
}

QTEST_GUILESS_MAIN(CommitEngineTest)
#include "commit_engine_test.moc"
