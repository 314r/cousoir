#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "book_zip.h"
#include "calibre_db.h"
#include "epub_metadata.h"
#include "test_support.h"

// The library's metadata written into an EPUB's package as Calibre's writer
// writes it: each package of tests/data/epubmeta/packages with each set of
// metadata.json, compared byte for byte with what Calibre made of them
// (tools/oracle/epub_meta_oracle.py), and a book written whole.

namespace {

const QString folder = QStringLiteral(COUSOIR_TEST_DATA_DIR "/epubmeta");

EpubMetadata::Metadata metadataFrom(const QJsonObject &json)
{
    EpubMetadata::Metadata metadata;
    metadata.title = json.value(QLatin1String("title")).toString();
    metadata.titleSort = json.value(QLatin1String("title_sort")).toString();
    for (const QJsonValue &author : json.value(QLatin1String("authors")).toArray())
        metadata.authors.append(author.toString());
    metadata.authorSort = json.value(QLatin1String("author_sort")).toString();
    for (const QJsonValue &language : json.value(QLatin1String("languages")).toArray())
        metadata.languages.append(language.toString());
    metadata.pubdate = CalibreDb::parseDate(json.value(QLatin1String("pubdate")).toVariant());
    metadata.comments = json.value(QLatin1String("comments")).toString();
    metadata.publisher = json.value(QLatin1String("publisher")).toString();
    for (const QJsonValue &tag : json.value(QLatin1String("tags")).toArray())
        metadata.tags.append(tag.toString());
    metadata.rating = json.value(QLatin1String("rating")).toInt();
    metadata.series = json.value(QLatin1String("series")).toString();
    metadata.seriesIndex = json.value(QLatin1String("series_index")).toDouble();
    for (const QJsonValue &pair : json.value(QLatin1String("identifiers")).toArray())
        metadata.identifiers.append({ pair.toArray().at(0).toString(), pair.toArray().at(1).toString() });
    metadata.applicationId = json.value(QLatin1String("application_id")).toInt();
    metadata.uuid = json.value(QLatin1String("uuid")).toString();
    // In their order, which the OPF 2 writer keeps.
    const PythonJson::Value all = PythonJson::parse(readAll(folder + QStringLiteral("/metadata.json")));
    const PythonJson::Value *set = all.find(json.value(QLatin1String("name")).toString());
    const PythonJson::Value *user = set ? set->find(QStringLiteral("user_metadata")) : nullptr;
    if (user) {
        for (const auto &[key, field] : user->fields) {
            metadata.userMetadata.append({ key, field });
            metadata.customKeys.append(key);
        }
    }
    if (json.value(QLatin1String("cover")).toBool())
        metadata.cover = QByteArrayLiteral("\xff\xd8\xff\xe0" "cover");
    metadata.interfaceLanguage = QStringLiteral("en");
    return metadata;
}

} // namespace

class EpubMetadataTest : public QObject
{
    Q_OBJECT

private slots:
    void writesPackagesAsCalibre_data();
    void writesPackagesAsCalibre();
    void leavesBrokenPackages();
    void writesABook();
};

void EpubMetadataTest::writesPackagesAsCalibre_data()
{
    QTest::addColumn<QString>("package");
    QTest::addColumn<QString>("set");
    const QJsonObject sets = QJsonDocument::fromJson(readAll(folder + QStringLiteral("/metadata.json"))).object();
    for (const QString &package : QDir(folder + QStringLiteral("/packages")).entryList({ QStringLiteral("*.opf") })) {
        for (const QString &set : sets.keys()) {
            const QString name = QFileInfo(package).completeBaseName() + u'-' + set;
            QTest::newRow(name.toUtf8().constData()) << package << set;
        }
    }
}

void EpubMetadataTest::writesPackagesAsCalibre()
{
    QFETCH(QString, package);
    QFETCH(QString, set);
    const QJsonObject sets = QJsonDocument::fromJson(readAll(folder + QStringLiteral("/metadata.json"))).object();
    const QString name = QFileInfo(package).completeBaseName() + u'-' + set;
    QJsonObject json = sets.value(set).toObject();
    json.insert(QLatin1String("name"), set);
    const std::optional<EpubMetadata::Package> result
        = EpubMetadata::apply(readAll(folder + QStringLiteral("/packages/") + package), metadataFrom(json), QStringLiteral("OEBPS"));
    QVERIFY(result);
    const QByteArray expected = readAll(folder + QStringLiteral("/expected/") + name + QStringLiteral(".opf"));
    if (result->opf != expected) {
        const QList<QByteArray> got = result->opf.split('\n');
        const QList<QByteArray> want = expected.split('\n');
        for (qsizetype i = 0; i < std::max(got.size(), want.size()); ++i) {
            if (got.value(i) != want.value(i)) {
                qWarning("line %lld\n  Calibre: %s\n  Cousoir: %s", qlonglong(i + 1), want.value(i).constData(),
                         got.value(i).constData());
                break;
            }
        }
    }
    QCOMPARE(result->opf, expected);
    const QJsonObject covers = QJsonDocument::fromJson(readAll(folder + QStringLiteral("/expected/covers.json"))).object();
    QCOMPARE(result->rasterCover, covers.value(name).toString());
}

void EpubMetadataTest::leavesBrokenPackages()
{
    EpubMetadata::Metadata metadata;
    metadata.title = QStringLiteral("T");
    QVERIFY(!EpubMetadata::apply("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"/>", metadata, {}));
    QVERIFY(!EpubMetadata::apply("no xml", metadata, {}));
}

void EpubMetadataTest::writesABook()
{
    // The fixture's book, with its package in OEBPS: the package changed,
    // the cover added where Calibre puts it, the rest as it was.
    QTemporaryDir temporary;
    const QString source = QStringLiteral(COUSOIR_TEST_DATA_DIR "/library/Glen Cook/The Black Company (2)/The Black Company - Glen Cook.epub");
    const QString target = temporary.filePath(QStringLiteral("sent.epub"));
    EpubMetadata::Metadata metadata;
    metadata.title = QStringLiteral("The Black Company");
    metadata.authors = { QStringLiteral("Glen Cook") };
    metadata.authorSort = QStringLiteral("Cook, Glen");
    metadata.applicationId = 2;
    metadata.cover = readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library/Glen Cook/The Black Company (2)/cover.jpg"));
    QVERIFY(!metadata.cover.isEmpty());
    QString error;
    QVERIFY2(EpubMetadata::write(source, target, metadata, &error), qPrintable(error));
    BookZip::Zip before;
    BookZip::Zip after;
    QVERIFY(BookZip::readZip(source, &before, &error));
    QVERIFY(BookZip::readZip(target, &after, &error));
    QCOMPARE(after.names.mid(0, before.names.size()), before.names);
    QCOMPARE(after.names.constLast(), QStringLiteral("OEBPS/OEBPS/cover.jpg"));
    QCOMPARE(after.data.value(QStringLiteral("OEBPS/OEBPS/cover.jpg")), metadata.cover);
    QVERIFY(after.data.value(QStringLiteral("OEBPS/content.opf")).contains("<dc:identifier>calibre:2</dc:identifier>"));
    QCOMPARE(after.data.value(QStringLiteral("OEBPS/text.xhtml")), before.data.value(QStringLiteral("OEBPS/text.xhtml")));

    // A file that is no EPUB is refused; one whose package cannot be
    // changed goes as it is.
    const QString text = temporary.filePath(QStringLiteral("a.txt"));
    QFile file(text);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("text");
    file.close();
    QVERIFY(!EpubMetadata::write(text, temporary.filePath(QStringLiteral("b.epub")), metadata, &error));
}

QTEST_GUILESS_MAIN(EpubMetadataTest)
#include "epub_metadata_test.moc"
