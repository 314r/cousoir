#include <QtTest>

#include <QTemporaryDir>

#include <zip.h>

#include "book_search.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "test_support.h"
#include "xml_tree.h"

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
    void searchesTheText();
    void findsTheSentences();
};

namespace {
// A book of two chapters for the search and the sentences: accents, a
// ligature, italics, white space, a soft hyphen, headings without white
// space before the paragraph after them, a style. After the test's class:
// moc misreads the raw strings before it.
bool writeTextEpub(const QString &path)
{
    const QByteArray chapter1 = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Un</title><style>p { color: red }</style></head>
<body><h1 id="un">L'Écume</h1>
<p>Les <i>jours</i> passent, deux
     mots et une lan&#173;terne.</p>
<h2 id="deux">Suite</h2><p>Une Œuvre, l'écume des JOURS.</p></body></html>)";
    const QByteArray chapter2 = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Deux</title></head>
<body><p>Encore l’écume, écume, écume.</p></body></html>)";
    return writeEpub(path, {
        { QStringLiteral("mimetype"), "application/epub+zip" },
        { QStringLiteral("META-INF/container.xml"), R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)" },
        { QStringLiteral("content.opf"), R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Recherche</dc:title><dc:identifier id="id">x</dc:identifier></metadata><manifest><item id="c1" href="c1.xhtml" media-type="application/xhtml+xml"/><item id="c2" href="c2.xhtml" media-type="application/xhtml+xml"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx"><itemref idref="c1"/><itemref idref="c2"/></spine></package>)" },
        { QStringLiteral("toc.ncx"), R"(<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap><navPoint id="n1"><navLabel><text>Premier</text></navLabel><content src="c1.xhtml#un"/><navPoint id="n2"><navLabel><text>Suite</text></navLabel><content src="c1.xhtml#deux"/></navPoint></navPoint><navPoint id="n3"><navLabel><text>Second</text></navLabel><content src="c2.xhtml"/></navPoint></navMap></ncx>)" },
        { QStringLiteral("c1.xhtml"), chapter1 },
        { QStringLiteral("c2.xhtml"), chapter2 },
    });
}

} // namespace

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

// A search of the book's text: case, accents and ligatures ignored, across
// elements, white space and soft hyphens; each place by the CFIs that name
// its text, under its chapter.
void EpubBookTest::searchesTheText()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("search.epub"));
    QVERIFY(writeTextEpub(path));
    QString error;
    std::shared_ptr<const EpubBook> book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    BookSearch search(book);

    // Each place's CFIs name the text found, in its page.
    const auto textOf = [&](const BookSearch::Match &match) {
        const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(book->read(book->spine().at(match.spineIndex).name));
        const EpubCfi::Document document(*root);
        return document.textBetween(document.decode(match.start), document.decode(match.end));
    };

    bool more = true;
    QList<BookSearch::Match> found = search.find(QStringLiteral("ecume"), 1000, &more);
    QVERIFY(!more);
    QCOMPARE(found.size(), 5);
    QCOMPARE(found.at(0).spineIndex, 0);
    QCOMPARE(found.at(0).text, QStringLiteral("Écume"));
    QCOMPARE(textOf(found.at(0)), QStringLiteral("Écume"));
    QCOMPARE(found.at(0).chapter, QStringLiteral("Premier"));
    QCOMPARE(found.at(1).text, QStringLiteral("écume"));
    QCOMPARE(found.at(1).chapter, QStringLiteral("Suite"));
    QCOMPARE(found.at(1).before, QStringLiteral("…deux mots et une lanterne. Suite Une Œuvre, l'"));
    QCOMPARE(found.at(1).after, QStringLiteral(" des JOURS."));
    QCOMPARE(found.at(2).spineIndex, 1);
    QCOMPARE(found.at(2).chapter, QStringLiteral("Second"));
    for (const BookSearch::Match &match : std::as_const(found))
        QCOMPARE(textOf(match).toCaseFolded(), match.text.toCaseFolded());

    // Across elements, blocks, white space and a soft hyphen; a ligature.
    found = search.find(QStringLiteral("suite une"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("SuiteUne"));
    found = search.find(QStringLiteral("les jours"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("Les jours"));
    found = search.find(QStringLiteral(" deux  mots "));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("deux\n     mots"));
    found = search.find(QStringLiteral("lanterne"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("lan­terne"));
    QCOMPARE(found.first().text, QStringLiteral("lanterne"));
    found = search.find(QStringLiteral("oeuvre"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("Œuvre"));
    // Not the style's text.
    QVERIFY(search.find(QStringLiteral("color")).isEmpty());

    // At most so many, and told so.
    found = search.find(QStringLiteral("écume"), 2, &more);
    QCOMPARE(found.size(), 2);
    QVERIFY(more);
}

// The sentences read aloud: each block on its own, those with no letter
// left out, each with the CFIs naming its text; the one a place is in.
void EpubBookTest::findsTheSentences()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("sentences.epub"));
    QVERIFY(writeTextEpub(path));
    QString error;
    const std::unique_ptr<EpubBook> book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    const BookSentences sentences(*book, 0);
    QStringList texts;
    for (const BookSentences::Sentence &sentence : sentences.sentences())
        texts.append(sentence.text);
    QCOMPARE(texts, (QStringList{ QStringLiteral("L'Écume"), QStringLiteral("Les jours passent, deux mots et une lanterne."),
                                  QStringLiteral("Suite"), QStringLiteral("Une Œuvre, l'écume des JOURS.") }));
    const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(book->read(book->spine().at(0).name));
    const EpubCfi::Document document(*root);
    for (const BookSentences::Sentence &sentence : sentences.sentences()) {
        QString text = document.textBetween(document.decode(sentence.start), document.decode(sentence.end));
        text.remove(QChar(0x00ad));
        QCOMPARE(text.simplified(), sentence.text);
    }
    // A place in the second sentence, and before all of them.
    const BookSentences::Sentence &second = sentences.sentences().at(1);
    QCOMPARE(sentences.sentenceAt(sentences.cfiAt(second.at + 10)), 1);
    QCOMPARE(sentences.sentenceAt(sentences.cfiAt(sentences.sentences().at(3).at)), 3);
    QCOMPARE(sentences.sentenceAt(QStringLiteral("/2")), 0);
    QCOMPARE(sentences.sentenceAt(QString()), 0);
    // The second chapter: one block, three sentences; a file that is not.
    QCOMPARE(BookSentences(*book, 1).sentences().size(), 1);
    QVERIFY(BookSentences(*book, 7).sentences().isEmpty());
}

#include "epub_book_test.moc"
