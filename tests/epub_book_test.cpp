#include <QtTest>

#include <QTemporaryDir>

#include <zip.h>

#include "epub_book.h"
#include "test_support.h"

namespace {

// An EPUB of `files`, by their names in the archive.
bool writeEpub(const QString &path, const QList<QPair<QString, QByteArray>> &files)
{
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &code);
    if (!archive)
        return false;
    QList<QByteArray> kept;
    for (const auto &[name, data] : files) {
        kept.append(data);
        zip_source_t *source = zip_source_buffer(archive, kept.last().constData(), zip_uint64_t(kept.last().size()), 0);
        if (!source || zip_file_add(archive, name.toUtf8().constData(), source, ZIP_FL_OVERWRITE) < 0)
            return false;
    }
    return zip_close(archive) == 0;
}

} // namespace

class EpubBookTest : public QObject
{
    Q_OBJECT

private slots:
    void readsAnEpub3();
    void readsAnEpub2();
    void resolvesPaths();
    void refusesWhatIsNoEpub();
};

void EpubBookTest::readsAnEpub3()
{
    QString error;
    const auto book = EpubBook::open(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub/rich.epub"), &error);
    QVERIFY2(book, qPrintable(error));
    QCOMPARE(book->title(), QStringLiteral("L'Écume des jours"));
    QCOMPARE(book->language(), QStringLiteral("fr"));
    QCOMPARE(book->spine().size(), 2);
    QCOMPARE(book->spine().at(1).name, QStringLiteral("OEBPS/text/chapter 1.xhtml"));
    QCOMPARE(book->spine().at(1).size, 1336);
    QCOMPARE(book->toc().size(), 1);
    QCOMPARE(book->toc().first().title, QStringLiteral("Chapitre premier"));
    QCOMPARE(book->toc().first().name, QStringLiteral("OEBPS/text/chapter 1.xhtml"));
    QCOMPARE(book->spineIndex(QStringLiteral("OEBPS/text/chapter 1.xhtml")), 1);
    QVERIFY(book->read(QStringLiteral("OEBPS/text/chapter 1.xhtml")).contains("<body"));
    QCOMPARE(book->mediaType(QStringLiteral("OEBPS/style.css")), QStringLiteral("text/css"));
    QCOMPARE(book->mediaType(QStringLiteral("OEBPS/images/plate.png")), QStringLiteral("image/png"));
    QVERIFY(book->read(QStringLiteral("OEBPS/missing.xhtml")).isEmpty());
}

void EpubBookTest::readsAnEpub2()
{
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("book.epub"));
    const QByteArray chapter = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>Text</p></body></html>";
    const QList<QPair<QString, QByteArray>> files{
        { QStringLiteral("mimetype"), "application/epub+zip" },
        { QStringLiteral("META-INF/container.xml"),
          R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
             <rootfiles><rootfile full-path="content/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
             </container>)" },
        { QStringLiteral("content/package.opf"),
          R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0">
             <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title> The   Black
             Company </dc:title><dc:language>en</dc:language></metadata>
             <manifest>
               <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
               <item id="c1" href="../text/one.html" media-type="application/xhtml+xml"/>
               <item id="c2" href="../text/two.html" media-type="application/xhtml+xml"/>
               <item id="notes" href="../text/notes.html" media-type="application/xhtml+xml"/>
               <item id="lost" href="../text/lost.html" media-type="application/xhtml+xml"/>
             </manifest>
             <spine toc="ncx" page-progression-direction="ltr"><itemref idref="c1"/><itemref idref="lost"/>
             <itemref idref="c2"/><itemref idref="notes" linear="no"/></spine></package>)" },
        { QStringLiteral("content/toc.ncx"),
          R"(<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>
             <navPoint id="a"><navLabel><text>Book One</text></navLabel><content src="../text/one.html"/>
               <navPoint id="b"><navLabel><text>The Company</text></navLabel><content src="../text/one.html#company"/></navPoint>
               <navPoint id="c"><navLabel><text>Croaker</text></navLabel><content src="../text/two.html"/></navPoint>
             </navPoint>
             <navPoint id="d"><navLabel><text>Notes</text></navLabel><content src="../text/notes.html"/></navPoint>
             </navMap></ncx>)" },
        { QStringLiteral("text/one.html"), chapter },
        { QStringLiteral("text/two.html"), chapter },
        { QStringLiteral("text/notes.html"), chapter },
    };
    QVERIFY(writeEpub(path, files));
    QString error;
    const auto book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    QCOMPARE(book->title(), QStringLiteral("The Black Company"));
    QCOMPARE(book->direction(), QStringLiteral("ltr"));
    // The item missing from the archive is left out.
    QCOMPARE(book->spine().size(), 3);
    QCOMPARE(book->spine().at(0).name, QStringLiteral("text/one.html"));
    QVERIFY(!book->spine().at(2).linear);
    QCOMPARE(book->toc().size(), 4);
    QCOMPARE(book->toc().at(1).title, QStringLiteral("The Company"));
    QCOMPARE(book->toc().at(1).name, QStringLiteral("text/one.html"));
    QCOMPARE(book->toc().at(1).fragment, QStringLiteral("company"));
    QCOMPARE(book->toc().at(1).depth, 1);
    QCOMPARE(book->toc().at(3).depth, 0);
    QCOMPARE(book->read(QStringLiteral("text/two.html")), chapter);
}

void EpubBookTest::resolvesPaths()
{
    QCOMPARE(EpubBook::resolve(QStringLiteral("OEBPS/Text/c.xhtml"), QStringLiteral("../Images/a%20b.png")),
             QStringLiteral("OEBPS/Images/a b.png"));
    QCOMPARE(EpubBook::resolve(QStringLiteral("OEBPS/Text/c.xhtml"), QStringLiteral("#note")),
             QStringLiteral("OEBPS/Text/c.xhtml"));
    QCOMPARE(EpubBook::resolve(QStringLiteral("c.xhtml"), QStringLiteral("d.xhtml#x")), QStringLiteral("d.xhtml"));
    QCOMPARE(EpubBook::resolve(QStringLiteral("a/b.xhtml"), QStringLiteral("../../../x.css")), QStringLiteral("x.css"));
}

void EpubBookTest::refusesWhatIsNoEpub()
{
    QTemporaryDir folder;
    QString error;
    QVERIFY(!EpubBook::open(folder.filePath(QStringLiteral("none.epub")), &error));
    QVERIFY(!error.isEmpty());
    const QString empty = folder.filePath(QStringLiteral("empty.epub"));
    QVERIFY(writeEpub(empty, { { QStringLiteral("mimetype"), "application/epub+zip" } }));
    QVERIFY(!EpubBook::open(empty, &error));
}

QTEST_GUILESS_MAIN(EpubBookTest)

#include "epub_book_test.moc"
