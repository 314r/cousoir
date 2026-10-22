#include <QtTest>

#include <QTemporaryDir>

#include "calibre_db.h"
#include "opf.h"
#include "test_support.h"

namespace {

// What Calibre 9.16 signs its files with.
const Opf::Settings calibreSettings{ QStringLiteral("fr"), QStringLiteral("calibre (9.16.0) [https://calibre-ebook.com]"),
                                     QStringLiteral("calibre") };

} // namespace

class OpfTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void writesWhatCalibreWrites_data();
    void writesWhatCalibreWrites();
    void writesCustomColumns_data();
    void writesCustomColumns();
    void keepsWhatItDoesNotWrite();
    void escapesAndLinks();
    void defaultsTheLanguage();

private:
    LibraryData m_data;
    LibraryData m_custom;
    QTemporaryDir m_cache;
};

void OpfTest::initTestCase()
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    QString error;
    QVERIFY2(CalibreDb::loadLibrary(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library"),
                                    m_cache.filePath(QStringLiteral("snapshot.db")), config, &m_data, &error),
             qPrintable(error));
    QVERIFY2(CalibreDb::loadLibrary(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library-custom"),
                                    m_cache.filePath(QStringLiteral("custom.db")), config, &m_custom, &error),
             qPrintable(error));
}

void OpfTest::writesCustomColumns_data()
{
    QTest::addColumn<int>("index");
    for (int index = 0; index < m_custom.books.size(); ++index)
        QTest::addRow("%s", qPrintable(m_custom.books.at(index).title)) << index;
}

void OpfTest::writesCustomColumns()
{
    // tools/make_custom_library.sh had Calibre write every metadata.opf, with
    // the custom columns' metadata and values; a composite's comes from the
    // file it replaces.
    QFETCH(int, index);
    const Book &book = m_custom.books.at(index);
    const QString path = QDir(m_custom.libraryPath).filePath(book.path + QStringLiteral("/metadata.opf"));
    const QByteArray expected = readAll(path);
    QVERIFY(!expected.isEmpty());
    QCOMPARE(QString::fromUtf8(Opf::metadataToOpf(m_custom, book, calibreSettings, expected)),
             QString::fromUtf8(expected));
}

void OpfTest::writesWhatCalibreWrites_data()
{
    QTest::addColumn<int>("index");
    for (int index = 0; index < m_data.books.size(); ++index)
        QTest::addRow("%s", qPrintable(m_data.books.at(index).title)) << index;
}

void OpfTest::writesWhatCalibreWrites()
{
    // tools/make_fixture_library.sh had Calibre write every metadata.opf.
    QFETCH(int, index);
    const Book &book = m_data.books.at(index);
    const QString path = QDir(m_data.libraryPath).filePath(book.path + QStringLiteral("/metadata.opf"));
    const QByteArray expected = readAll(path);
    QVERIFY(!expected.isEmpty());
    QCOMPARE(QString::fromUtf8(Opf::metadataToOpf(m_data, book, calibreSettings, expected)),
             QString::fromUtf8(expected));
}

void OpfTest::keepsWhatItDoesNotWrite()
{
    const QByteArray previous = R"(<?xml version='1.0' encoding='utf-8'?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="uuid_id" version="2.0">
    <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
        <meta name="calibre:rating" content="2"/>
        <meta name="calibre:user_metadata:#read" content="{&quot;datatype&quot;: &quot;bool&quot;}"/>
        <meta name="calibre:annotation" content="{&quot;type&quot;: &quot;bookmark&quot;}"/>
    </metadata>
</package>
)";
    const QString opf = QString::fromUtf8(Opf::metadataToOpf(m_data, m_data.books.first(), calibreSettings, previous));
    // A column the library no longer has goes, as Calibre writes only its
    // own; the annotations stay.
    QVERIFY(!opf.contains(QStringLiteral("user_metadata")));
    QVERIFY(opf.contains(QStringLiteral("name=\"calibre:annotation\"")));
    // Its own fields come from the database, not the old file.
    QVERIFY(opf.contains(QStringLiteral("<meta name=\"calibre:rating\" content=\"8\"/>")));
    QVERIFY(!opf.contains(QStringLiteral("content=\"2\"")));
    QVERIFY(opf.indexOf(QStringLiteral("calibre:title_sort")) < opf.indexOf(QStringLiteral("calibre:annotation")));
}

void OpfTest::escapesAndLinks()
{
    LibraryData data = m_data;
    Book book = data.books.first();
    book.title = QStringLiteral("A & B <C> \"D\"\x01");
    book.publisher = QStringLiteral("Line\nbreak");
    for (FacetItem &author : data.authors) {
        if (author.id == book.authorIds.first())
            author.link = QStringLiteral("https://example.org/?a=1&b=\"2\"");
    }
    const QString opf = QString::fromUtf8(Opf::metadataToOpf(data, book, calibreSettings));
    QVERIFY(opf.contains(QStringLiteral("<dc:title>A &amp; B &lt;C&gt; \"D\"</dc:title>")));
    QVERIFY(opf.contains(QStringLiteral("<dc:publisher>Line\nbreak</dc:publisher>")));
    QVERIFY(opf.contains(QStringLiteral(
        "<meta name=\"calibre:link_maps\" content=\"{&quot;authors&quot;: {&quot;Boris Vian&quot;: "
        "&quot;https://example.org/?a=1&amp;b=\\&quot;2\\&quot;&quot;}}\"/>")));
}

void OpfTest::defaultsTheLanguage()
{
    Book book = m_data.books.first();
    book.languages.clear();
    Opf::Settings settings = calibreSettings;
    settings.interfaceLanguage = QStringLiteral("pt_BR");
    const QString opf = QString::fromUtf8(Opf::metadataToOpf(m_data, book, settings));
    QVERIFY(opf.contains(QStringLiteral("<dc:language>pt</dc:language>")));
    QVERIFY(opf.contains(QStringLiteral("title=\"Capa\"")));
}

QTEST_GUILESS_MAIN(OpfTest)
#include "opf_test.moc"
