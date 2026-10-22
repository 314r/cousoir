#include <QtTest>

#include <QDirIterator>
#include <QTemporaryDir>

#include "calibre_db.h"
#include "library_check.h"
#include "test_support.h"

namespace {

bool writeFile(const QString &path, const QByteArray &contents = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

LibraryCheck::Options options()
{
    LibraryCheck::Options result;
    result.interfaceLanguage = QStringLiteral("fr");
    return result;
}

// A library of authors only, each with as many books as its place from the
// end, and no folder.
LibraryData authorsOnly(const QStringList &names)
{
    LibraryData data;
    for (qsizetype i = 0; i < names.size(); ++i) {
        const QString sort = CalibreText::authorToAuthorSort(names.at(i), CalibreText::Tweaks::defaults());
        data.authors.append({ int(i + 1), names.at(i), sort, int(names.size() - i), {} });
    }
    return data;
}

QList<QStringList> variantNames(const LibraryData &data, const LibraryCheck::Findings &findings)
{
    QList<QStringList> result;
    for (const LibraryCheck::AuthorGroup &group : findings.authorVariants) {
        QStringList names;
        for (const int id : group.authors) {
            for (const FacetItem &author : data.authors) {
                if (author.id == id)
                    names.append(author.name);
            }
        }
        result.append(names);
    }
    return result;
}

Book book(int id, const QString &title, const QList<int> &authorIds, const QStringList &authors,
          const QStringList &languages = {})
{
    Book result;
    result.id = id;
    result.title = title;
    result.authorIds = authorIds;
    result.authors = authors;
    result.languages = languages;
    result.timestamp = QDateTime(QDate(2024, 1, id), QTime(12, 0), QTimeZone::UTC);
    return result;
}

} // namespace

class LibraryCheckTest : public QObject
{
    Q_OBJECT

private slots:
    void foldsNames_data();
    void foldsNames();
    void matchesInitials_data();
    void matchesInitials();
    void groupsAuthorVariants_data();
    void groupsAuthorVariants();
    void reportsCaseDuplicates();
    void findsDuplicateBooks();
    void keysIsbns();
    void findsMissingFiles();
    void findsFilesOutside();
    void buildsFixes();

private:
    LibraryData load(const QString &path);

    QTemporaryDir m_cache;
};

LibraryData LibraryCheckTest::load(const QString &path)
{
    LibraryData data;
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    QString error;
    if (!CalibreDb::loadLibrary(path, m_cache.filePath(QStringLiteral("snapshot.db")), config, &data, &error))
        qWarning() << error;
    return data;
}

void LibraryCheckTest::foldsNames_data()
{
    QTest::addColumn<QString>("first");
    QTest::addColumn<QString>("second");
    QTest::addColumn<bool>("same");
    const auto row = [](const char *first, const char *second, bool same) {
        QTest::newRow(qPrintable(QStringLiteral("%1 | %2").arg(QString::fromUtf8(first), QString::fromUtf8(second))))
            << QString::fromUtf8(first) << QString::fromUtf8(second) << same;
    };
    row("Boris Vian", "Vian, Boris", true);
    row("Boris Vian", "bóris  vián", true);
    row("Boris Vian", "Vian Boris", true);
    row("J.R.R. Tolkien", "Tolkien, J. R. R.", true);
    row("Jean-Pierre Dupont", "Jean Pierre Dupont", true);
    row("Flannery O'Connor", "Flannery OConnor", true);
    row("John Smith", "Jane Smith", false);
    row("Martin Luther King", "Martin Luther King Jr.", false);
    // Other scripts stay as they are: two names that read alike in Latin
    // letters are two names.
    row("王伟", "汪威", false);
}

void LibraryCheckTest::foldsNames()
{
    QFETCH(QString, first);
    QFETCH(QString, second);
    QFETCH(bool, same);
    QCOMPARE(LibraryCheck::authorKey(first) == LibraryCheck::authorKey(second), same);
}

void LibraryCheckTest::matchesInitials_data()
{
    QTest::addColumn<QString>("first");
    QTest::addColumn<QString>("second");
    QTest::addColumn<bool>("same");
    const auto row = [](const char *first, const char *second, bool same) {
        QTest::newRow(qPrintable(QStringLiteral("%1 | %2").arg(QString::fromUtf8(first), QString::fromUtf8(second))))
            << QString::fromUtf8(first) << QString::fromUtf8(second) << same;
    };
    row("B. Vian", "Boris Vian", true);
    row("Vian, B.", "Boris Vian", true);
    row("J. R. R. Tolkien", "John Ronald Reuel Tolkien", true);
    row("George R. R. Martin", "George Martin", false);
    row("John Smith", "Jane Smith", false);
    row("Boris Vian", "Boris Vian", false);
    row("B. V.", "Boris V.", false);
    row("Plato", "P.", false);
}

void LibraryCheckTest::matchesInitials()
{
    QFETCH(QString, first);
    QFETCH(QString, second);
    QFETCH(bool, same);
    QCOMPARE(LibraryCheck::sameByInitials(first, second, CalibreText::Tweaks::defaults()), same);
}

void LibraryCheckTest::groupsAuthorVariants_data()
{
    QTest::addColumn<QStringList>("names");
    QTest::addColumn<QList<QStringList>>("groups");
    QTest::addColumn<bool>("initials");
    QTest::newRow("vian") << QStringList{ "Boris Vian", "Vian, Boris", "B. Vian", "Glen Cook" }
                          << QList<QStringList>{ { "Boris Vian", "Vian, Boris", "B. Vian" } } << true;
    QTest::newRow("written differently") << QStringList{ "Boris Vian", "Vian, Boris" }
                                         << QList<QStringList>{ { "Boris Vian", "Vian, Boris" } } << false;
    QTest::newRow("ambiguous initial") << QStringList{ "John Smith", "Jane Smith", "J. Smith" }
                                       << QList<QStringList>{} << false;
    QTest::newRow("one initial") << QStringList{ "John Smith", "J. Smith" }
                                 << QList<QStringList>{ { "John Smith", "J. Smith" } } << true;
    QTest::newRow("more given names") << QStringList{ "George R. R. Martin", "George Martin" }
                                      << QList<QStringList>{} << false;
}

void LibraryCheckTest::groupsAuthorVariants()
{
    QFETCH(QStringList, names);
    QFETCH(QList<QStringList>, groups);
    QFETCH(bool, initials);
    LibraryCheck::Options checks = options();
    checks.scanFiles = false;
    const LibraryData data = authorsOnly(names);
    const LibraryCheck::Findings findings = LibraryCheck::check(data, checks);
    QCOMPARE(variantNames(data, findings), groups);
    if (!groups.isEmpty())
        QCOMPARE(findings.authorVariants.first().initials, initials);
    QVERIFY(findings.caseDuplicates.isEmpty());
}

void LibraryCheckTest::reportsCaseDuplicates()
{
    LibraryCheck::Options checks = options();
    checks.scanFiles = false;
    const LibraryCheck::Findings findings = LibraryCheck::check(authorsOnly({ "Boris Vian", "boris vian" }), checks);
    QCOMPARE(findings.caseDuplicates, (QStringList{ "Boris Vian", "boris vian" }));
    QCOMPARE(findings.authorVariants.size(), 1);
}

void LibraryCheckTest::findsDuplicateBooks()
{
    LibraryData data = authorsOnly({ "Boris Vian", "Vian, Boris", "Glen Cook" });
    data.books = {
        book(1, QStringLiteral("L'Écume des jours"), { 1 }, { "Boris Vian" }, { "fra" }),
        // An article, case and an accent aside, under a variant: a copy.
        book(2, QStringLiteral("l'ecume des Jours"), { 2 }, { "Vian, Boris" }, { "fra" }),
        // The same title by someone else is another book.
        book(3, QStringLiteral("L'Écume des jours"), { 3 }, { "Glen Cook" }),
        // In another language too.
        book(4, QStringLiteral("L'Écume des jours"), { 1 }, { "Boris Vian" }, { "eng" }),
        // The same ISBN, written two ways.
        book(5, QStringLiteral("The Black Company"), { 3 }, { "Glen Cook" }),
        book(6, QStringLiteral("La Compagnie noire"), { 3 }, { "Glen Cook" }),
        // Unknown titles are no copies of each other.
        book(7, QStringLiteral("Inconnu(e)"), { 3 }, { "Glen Cook" }),
        book(8, QStringLiteral("Inconnu(e)"), { 3 }, { "Glen Cook" }),
        // A book of no language goes with those of the one language there is.
        book(9, QStringLiteral("Shadows Linger"), { 3 }, { "Glen Cook" }, { "eng" }),
        book(10, QStringLiteral("Shadows  Linger"), { 3 }, { "Glen Cook" }),
    };
    data.books[4].identifiers = { { "isbn", "0-8125-2139-0" } };
    data.books[5].identifiers = { { "ISBN", "978-0-8125-2139-9" } };
    // The copy with a file is the one to keep.
    data.books[1].formats = { { "EPUB", "x", 1, "/x.epub" } };
    LibraryCheck::Options checks = options();
    checks.scanFiles = false;
    const LibraryCheck::Findings findings = LibraryCheck::check(data, checks);
    // By title.
    QCOMPARE(findings.duplicates.size(), 3);
    QCOMPARE(findings.duplicates.at(0).books, (QList<int>{ 2, 1 }));
    QVERIFY(findings.duplicates.at(0).sameTitle);
    QVERIFY(findings.duplicates.at(0).isbns.isEmpty());
    QCOMPARE(findings.duplicates.at(1).books, (QList<int>{ 9, 10 }));
    QCOMPARE(findings.duplicates.at(2).books, (QList<int>{ 5, 6 }));
    QVERIFY(!findings.duplicates.at(2).sameTitle);
    QCOMPARE(findings.duplicates.at(2).isbns, QStringList{ "9780812521399" });
}

void LibraryCheckTest::keysIsbns()
{
    QCOMPARE(LibraryCheck::isbnKey(QStringLiteral("0-8125-2139-0")), QStringLiteral("9780812521399"));
    QCOMPARE(LibraryCheck::isbnKey(QStringLiteral("978-2-253-14087-0")), QStringLiteral("9782253140870"));
    QCOMPARE(LibraryCheck::isbnKey(QStringLiteral("0-8125-2139-2")), QString());
    QCOMPARE(LibraryCheck::isbnKey(QStringLiteral("none")), QString());
}

void LibraryCheckTest::findsMissingFiles()
{
    QTemporaryDir root;
    const QString library = root.filePath(QStringLiteral("library"));
    QVERIFY(copyTree(fixtureLibrary(), library));
    QVERIFY(QFile::remove(QDir(library).filePath(QStringLiteral("Glen Cook/The Black Company (2)/The Black Company - Glen Cook.azw3"))));
    // A file kept online only is not missing.
    const QString online = QDir(library).filePath(QStringLiteral("Glen Cook/Shadows Linger (3)/Shadows Linger - Glen Cook.epub"));
    QVERIFY(QFile::rename(online, online + QStringLiteral(".nextcloud")));
    LibraryCheck::Options checks = options();
    checks.scanFiles = false;
    const LibraryCheck::Findings findings = LibraryCheck::check(load(library), checks);
    QCOMPARE(findings.missing.size(), 2);
    QCOMPARE(findings.missing.at(0).book, 2);
    QCOMPARE(findings.missing.at(0).formats, QStringList{ "AZW3" });
    QVERIFY(!findings.missing.at(0).noFormats);
    // Le Petit Prince has no file at all.
    QCOMPARE(findings.missing.at(1).book, 6);
    QVERIFY(findings.missing.at(1).noFormats);
    QVERIFY(!findings.missing.at(1).folderMissing);
}

void LibraryCheckTest::findsFilesOutside()
{
    QTemporaryDir root;
    const QString library = root.filePath(QStringLiteral("library"));
    QVERIFY(copyTree(fixtureLibrary(), library));
    QCOMPARE(LibraryCheck::check(load(library), options()).outside.size(), 0);

    const QDir dir(library);
    for (const char *file : { "stray.epub", "Nobody/notes.txt", "Glen Cook/Lost (99)/Lost - Glen Cook.epub",
                              "Glen Cook/The Black Company (2)/old.epub", "Glen Cook/readme.txt" })
        QVERIFY(writeFile(dir.filePath(QString::fromUtf8(file))));
    // What a library holds besides its books.
    for (const char *file : { ".stfolder/x", ".~cousoir-1-old", "Thumbs.db",
                              "metadata (Pierre's conflicted copy 2024-11-14).db",
                              "Glen Cook/The Black Company (2)/data/extra.txt",
                              "Glen Cook/The Black Company (2)/.DS_Store" })
        QVERIFY(writeFile(dir.filePath(QString::fromUtf8(file))));
    // Calibre writing, after the library was read.
    const LibraryData data = load(library);
    QVERIFY(writeFile(dir.filePath(QStringLiteral("metadata.db-wal"))));

    const LibraryCheck::Findings findings = LibraryCheck::check(data, options());
    QVERIFY(findings.scanned);
    QStringList paths;
    for (const LibraryCheck::OutsideFile &file : findings.outside)
        paths.append(file.path + (file.folder ? QStringLiteral("/") : QString()));
    QCOMPARE(paths, (QStringList{ "Glen Cook/Lost (99)/", "Glen Cook/readme.txt",
                                  "Glen Cook/The Black Company (2)/old.epub", "Nobody/", "stray.epub" }));
    QCOMPARE(findings.outside.first().book, 99);
    QCOMPARE(findings.outside.last().size, 1);

    // Stopped half way, the walk says so.
    LibraryCheck::Options stopped = options();
    stopped.canceled = [] { return true; };
    QVERIFY(!LibraryCheck::check(data, stopped).scanned);
}

void LibraryCheckTest::buildsFixes()
{
    LibraryData data = authorsOnly({ "Terry Pratchett", "Pratchett, Terry", "Neil Gaiman" });
    data.authors[0].sort = QStringLiteral("Pratchett, Terry");
    data.authors[1].sort = QStringLiteral("Pratchett, Terry");
    data.authors[2].sort = QStringLiteral("Gaiman, Neil");
    data.books = {
        book(1, QStringLiteral("Good Omens"), { 2, 3 }, { "Pratchett, Terry", "Neil Gaiman" }),
        // Both names on one book: one stays.
        book(2, QStringLiteral("Strata"), { 2, 1 }, { "Pratchett, Terry", "Terry Pratchett" }),
        book(3, QStringLiteral("Mort"), { 1 }, { "Terry Pratchett" }),
    };
    data.books[0].authorSort = QStringLiteral("Pratchett, Terry & Gaiman, Neil");
    data.books[1].authorSort = QStringLiteral("Pratchett");

    QString error;
    const QList<BookEdit> edits = LibraryCheck::mergeAuthors(data, { 1, 2 }, 1, &error);
    QCOMPARE(edits.size(), 2);
    QCOMPARE(edits.at(0).bookId, 1);
    QCOMPARE(*edits.at(0).authors, (QStringList{ "Terry Pratchett", "Neil Gaiman" }));
    // A sort that followed the authors follows them still; another stays.
    QVERIFY(!edits.at(0).authorSort);
    QCOMPARE(edits.at(1).bookId, 2);
    QCOMPARE(*edits.at(1).authors, QStringList{ "Terry Pratchett" });
    QCOMPARE(*edits.at(1).authorSort, QStringLiteral("Pratchett"));
    QVERIFY(LibraryCheck::mergeAuthors(data, { 1, 2 }, 9, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("no longer")));

    CommitEngine::Request request;
    QVERIFY(LibraryCheck::keepOne(data, 3, { 1, 3, 2 }, &request, &error));
    QCOMPARE(request.trash, (QList<int>{ 1, 2 }));
    QCOMPARE(request.formatCopies.size(), 2);
    QCOMPARE(request.formatCopies.at(0).from, 1);
    QCOMPARE(request.formatCopies.at(0).to, 3);
    CommitEngine::Request none;
    QVERIFY(!LibraryCheck::keepOne(data, 3, { 3 }, &none, &error));
    QVERIFY(!LibraryCheck::keepOne(data, 3, { 42 }, &none, &error));
}

QTEST_GUILESS_MAIN(LibraryCheckTest)
#include "library_check_test.moc"
