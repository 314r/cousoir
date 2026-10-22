#include <QtTest>

#include <QDirIterator>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <sqlite3.h>
#include <zip.h>

#include "sqlite_db.h"
#include "test_support.h"

// cousoir-test-tool's comparisons must find what differs, or the golden
// tests that rely on them would pass whatever Cousoir does.

namespace {

struct Run
{
    int exitCode = -1;
    QString output;

    QString lastLine() const { return output.trimmed().section(u'\n', -1); }
};

Run tool(const QStringList &arguments)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral(COUSOIR_TEST_TOOL), arguments);
    if (!process.waitForFinished(60000))
        return {};
    return { process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
             QString::fromUtf8(process.readAll()) };
}

bool copyFixture(const QString &to)
{
    const QDir from(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library"));
    QDirIterator it(from.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString target = QDir(to).filePath(from.relativeFilePath(path));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(path, target))
            return false;
    }
    return true;
}

bool replaceIn(const QString &path, const QByteArray &before, const QByteArray &after)
{
    QByteArray data = readAll(path);
    if (!data.contains(before))
        return false;
    return writeAll(path, data.replace(before, after));
}

// Runs SQL on a library's database, with Calibre's title_sort() for the
// triggers of the books table.
bool execute(const QString &library, const QString &sql)
{
    SqliteDb db;
    QString error;
    if (!db.open(QDir(library).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error))
        return false;
    sqlite3_create_function(
        db.handle(), "title_sort", 1, SQLITE_UTF8, nullptr,
        [](sqlite3_context *context, int, sqlite3_value **values) { sqlite3_result_value(context, values[0]); },
        nullptr, nullptr);
    if (!db.exec(sql, &error)) {
        qWarning() << error;
        return false;
    }
    return true;
}

bool writeZip(const QString &path, const QList<std::pair<QString, QByteArray>> &entries)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    int error = 0;
    zip_t *zip = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!zip)
        return false;
    for (const auto &[name, data] : entries) {
        zip_source_t *source = zip_source_buffer(zip, data.constData(), zip_uint64_t(data.size()), 0);
        if (!source || zip_file_add(zip, name.toUtf8().constData(), source, ZIP_FL_ENC_UTF_8) < 0) {
            zip_discard(zip);
            return false;
        }
    }
    return zip_close(zip) == 0;
}

const QString blackCompany = QStringLiteral("Glen Cook/The Black Company (2)");
const QString goodOmens = QStringLiteral("Terry Pratchett/Good Omens (4)");

} // namespace

class TestToolTest : public QObject
{
    Q_OBJECT

private slots:
    void comparesLibraries()
    {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(copyFixture(a.path()) && copyFixture(b.path()));
        Run run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 0);
        QVERIFY2(run.lastLine().endsWith(QLatin1String(" files compared, 0 differences")), qPrintable(run.output));
        QVERIFY(run.lastLine().startsWith(QLatin1String("28 tables and ")));

        // A row, a book file and a file only on one side.
        QVERIFY(execute(b.path(), QStringLiteral("UPDATE publishers SET name = 'Folio' WHERE name = 'Gallimard'")));
        const QString epub = QDir(b.path()).filePath(blackCompany + QStringLiteral("/The Black Company - Glen Cook.epub"));
        QVERIFY(writeAll(epub, readAll(epub) + 'x'));
        QVERIFY(writeAll(QDir(b.path()).filePath(QStringLiteral("stray.txt")), "stray"));
        run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY2(run.output.contains(QLatin1String("table publishers differs:\n"
                                                    "    Calibre only: [(1, 'Gallimard', None, '')]\n"
                                                    "    Cousoir only: [(1, 'Folio', None, '')]")),
                 qPrintable(run.output));
        QVERIFY(run.output.contains(blackCompany + QStringLiteral("/The Black Company - Glen Cook.epub differs")));
        QVERIFY(run.output.contains(QLatin1String("file stray.txt only in the Cousoir copy")));
        QVERIFY(run.lastLine().endsWith(QLatin1String(", 3 differences")));

        // Only the files.
        run = tool({ QStringLiteral("compare-libraries"), QStringLiteral("--files-only"), a.path(), b.path() });
        QVERIFY(run.lastLine().startsWith(QLatin1String("0 tables and ")));
        QVERIFY(run.lastLine().endsWith(QLatin1String(", 2 differences")));
    }

    void comparesPackageFilesAsXml()
    {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(copyFixture(a.path()) && copyFixture(b.path()));
        const QString opf = QDir(b.path()).filePath(blackCompany + QStringLiteral("/metadata.opf"));
        // Attributes in another order and space around the text are the same.
        QVERIFY(replaceIn(opf, R"(<dc:identifier opf:scheme="calibre" id="calibre_id">2</dc:identifier>)",
                          R"(<dc:identifier id="calibre_id"   opf:scheme="calibre">2</dc:identifier>)"));
        QVERIFY(replaceIn(opf, "<dc:publisher>Tor</dc:publisher>", "<dc:publisher>\n  Tor </dc:publisher>"));
        Run run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));

        QVERIFY(replaceIn(opf, "<dc:title>The Black Company</dc:title>", "<dc:title>The White Company</dc:title>"));
        run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY(run.output.contains(blackCompany + QStringLiteral("/metadata.opf differs:")));
        QVERIFY(run.output.contains(QLatin1String("'The White Company'")));
    }

    void comparesCoversByPixels()
    {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(copyFixture(a.path()) && copyFixture(b.path()));
        // Encoded again, the same picture.
        const QString cover = QDir(b.path()).filePath(blackCompany + QStringLiteral("/cover.jpg"));
        QImage image(cover);
        QVERIFY(!image.isNull());
        QVERIFY(image.save(cover, "JPEG", 70));
        Run run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));

        // Another picture, and another size.
        image.fill(Qt::red);
        QVERIFY(image.save(cover, "JPEG"));
        const QString other = QDir(b.path()).filePath(goodOmens + QStringLiteral("/cover.jpg"));
        QVERIFY(QImage(other).scaled(300, 450).save(other, "JPEG"));
        run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY2(run.output.contains(blackCompany + QStringLiteral("/cover.jpg: covers differ by")), qPrintable(run.output));
        QVERIFY(run.output.contains(goodOmens + QStringLiteral("/cover.jpg: 600x900 against 300x450")));
    }

    void leavesOutWhatBooksAddedGet()
    {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(copyFixture(a.path()) && copyFixture(b.path()));
        QVERIFY(execute(b.path(), QStringLiteral("UPDATE books SET uuid = '00000000-0000-4000-8000-000000000000', "
                                                 "timestamp = '2030-01-01 00:00:00+00:00', last_modified = "
                                                 "'2030-01-01 00:00:00+00:00' WHERE id = 6")));
        const QString opf = QDir(b.path()).filePath(QStringLiteral("Antoine de Saint-Exupery/Le Petit Prince (6)/metadata.opf"));
        QByteArray data = readAll(opf);
        static const QRegularExpression uuid(QStringLiteral("(opf:scheme=\"uuid\" id=\"uuid_id\">)[^<]*"));
        static const QRegularExpression timestamp(QStringLiteral("(name=\"calibre:timestamp\" content=\")[^\"]*"));
        QString text = QString::fromUtf8(data);
        QVERIFY(text.contains(uuid) && text.contains(timestamp));
        text.replace(uuid, QStringLiteral("\\1new-uuid")).replace(timestamp, QStringLiteral("\\12030-01-01T00:00:00+00:00"));
        QVERIFY(writeAll(opf, text.toUtf8()));

        Run run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY(run.output.contains(QLatin1String("table books differs:")));
        QVERIFY(run.output.contains(QLatin1String("Le Petit Prince (6)/metadata.opf differs:")));
        run = tool({ QStringLiteral("compare-libraries"), QStringLiteral("--added-from"), QStringLiteral("6"), a.path(),
                     b.path() });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));
        run = tool({ QStringLiteral("compare-libraries"), QStringLiteral("--added-from"), QStringLiteral("7"), a.path(),
                     b.path() });
        QCOMPARE(run.exitCode, 1);
    }

    void comparesConversionsByContent()
    {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(copyFixture(a.path()) && copyFixture(b.path()));
        // The same conversion done twice: its time, the ids drawn for its
        // anchors, its classes and the order of its entries but the first
        // differ.
        const auto conversion = [](const QByteArray &time, const QByteArray &id, const QByteArray &classes,
                                   bool reversed) {
            QList<std::pair<QString, QByteArray>> rest{
                { QStringLiteral("content.opf"),
                  "<package><meta property=\"dcterms:modified\">" + time + "</meta></package>" },
                { QStringLiteral("text.html"), "<p class=\"" + classes + "\" id=\"" + id + "\">Text</p>" },
            };
            if (reversed)
                std::reverse(rest.begin(), rest.end());
            return QList<std::pair<QString, QByteArray>>{ { QStringLiteral("mimetype"), "application/epub+zip" } }
                   + rest;
        };
        const QString file = goodOmens + QStringLiteral("/Good Omens - Terry Pratchett.epub");
        QVERIFY(writeZip(QDir(a.path()).filePath(file),
                         conversion("2026-01-01T00:00:00Z", "0123456789abcdef0123456789abcdef", "b a", false)));
        QVERIFY(writeZip(QDir(b.path()).filePath(file),
                         conversion("2026-02-02T00:00:00Z", "fedcba9876543210fedcba9876543210", "a  b", true)));
        Run run = tool({ QStringLiteral("compare-libraries"), QStringLiteral("--converted"), QStringLiteral("4:EPUB"),
                         a.path(), b.path() });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));
        run = tool({ QStringLiteral("compare-libraries"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY(run.output.contains(file + QStringLiteral(" differs")));

        QVERIFY(writeZip(QDir(b.path()).filePath(file),
                         conversion("2026-02-02T00:00:00Z", "fedcba9876543210fedcba9876543210", "a c", true)));
        run = tool({ QStringLiteral("compare-libraries"), QStringLiteral("--converted"), QStringLiteral("4:EPUB"),
                     a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY(run.output.contains(file + QStringLiteral(" differs as a conversion:")));
    }

    void comparesDevices()
    {
        QTemporaryDir a;
        QTemporaryDir b;
        const auto device = [](const QString &root, const QString &time, const QString &title) {
            const QJsonArray books{ QJsonObject{
                { QStringLiteral("lpath"), QStringLiteral("Cook, Glen/Black Company.kepub.epub") },
                { QStringLiteral("title"), title },
                { QStringLiteral("cover"), root + QStringLiteral("/tmp/cover.jpg") },
                { QStringLiteral("size"), 1000 + title.size() },
            } };
            const QJsonObject driveinfo{
                { QStringLiteral("date_last_connected"), time },
                { QStringLiteral("device_store_uuid"), root },
                { QStringLiteral("prefix"), root + u'/' },
            };
            return writeAll(QDir(root).filePath(QStringLiteral("metadata.calibre")), QJsonDocument(books).toJson())
                   && writeAll(QDir(root).filePath(QStringLiteral("driveinfo.calibre")), QJsonDocument(driveinfo).toJson())
                   && writeZip(QDir(root).filePath(QStringLiteral("Cook, Glen/Black Company.kepub.epub")),
                               { { QStringLiteral("mimetype"), "application/epub+zip" },
                                 { QStringLiteral("content.opf"),
                                   "<meta property=\"dcterms:modified\">" + time.toUtf8() + "</meta>" } })
                   && writeAll(QDir(root).filePath(QStringLiteral("Manual.pdf")),
                               "%PDF-1.4 <xmp:MetadataDate>" + time.toUtf8() + "</xmp:MetadataDate>")
                   && writeAll(QDir(root).filePath(QStringLiteral(".kobo/KoboReader.sqlite")), time.toUtf8());
        };
        QVERIFY(device(a.path(), QStringLiteral("2026-01-01"), QStringLiteral("The Black Company")));
        QVERIFY(device(b.path(), QStringLiteral("2026-02-02"), QStringLiteral("The Black Company")));
        Run run = tool({ QStringLiteral("compare-devices"), a.path(), b.path() });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));
        // The Kobo's database is left out.
        QCOMPARE(run.lastLine(), QStringLiteral("4 files compared, 0 differences"));

        QVERIFY(device(b.path(), QStringLiteral("2026-02-02"), QStringLiteral("The White Company")));
        run = tool({ QStringLiteral("compare-devices"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY2(run.output.contains(QLatin1String("metadata.calibre: Cook, Glen/Black Company.kepub.epub: title differs:\n"
                                                    "    Calibre: \"The Black Company\"\n"
                                                    "    Cousoir: \"The White Company\"")),
                 qPrintable(run.output));
        run = tool({ QStringLiteral("compare-devices"), QStringLiteral("--lpaths-only"), a.path(), b.path() });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));

        QVERIFY(writeAll(QDir(b.path()).filePath(QStringLiteral("Manual.pdf")), "%PDF-1.4 changed"));
        run = tool({ QStringLiteral("compare-devices"), QStringLiteral("--lpaths-only"), a.path(), b.path() });
        QCOMPARE(run.exitCode, 1);
        QVERIFY(run.output.contains(QLatin1String("Manual.pdf differs")));
    }

    void comparesReads()
    {
        QTemporaryDir folder;
        const auto reads = [&folder](const QString &name, const QJsonArray &books) {
            const QString path = folder.filePath(name);
            return writeAll(path, QJsonDocument(books).toJson()) ? path : QString();
        };
        const QJsonObject book{
            { QStringLiteral("file"), QStringLiteral("a.epub") },
            { QStringLiteral("title"), QStringLiteral("A") },
            { QStringLiteral("series_index"), 1 },
            { QStringLiteral("identifiers"), QJsonObject{ { QStringLiteral("isbn"), QStringLiteral("1") } } },
        };
        QJsonObject same = book;
        same.insert(QStringLiteral("series_index"), 1.0);
        QJsonObject other = book;
        other.insert(QStringLiteral("title"), QStringLiteral("B"));
        const QString calibre = reads(QStringLiteral("calibre.json"), { book });
        Run run = tool({ QStringLiteral("compare-reads"), calibre, reads(QStringLiteral("same.json"), { same }) });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));
        QCOMPARE(run.lastLine(), QStringLiteral("1 files, 0 differences"));
        run = tool({ QStringLiteral("compare-reads"), calibre, reads(QStringLiteral("other.json"), { other }) });
        QCOMPARE(run.exitCode, 1);
        QVERIFY(run.output.contains(QLatin1String("a.epub: title\n    calibre \"A\"\n    cousoir \"B\"")));
        QCOMPARE(run.lastLine(), QStringLiteral("1 files, 1 differences {'title': 1}"));
        run = tool({ QStringLiteral("compare-reads"), calibre, reads(QStringLiteral("two.json"), { book, book }) });
        QCOMPARE(run.exitCode, 1);
    }

    void makesFixtureFiles()
    {
        QTemporaryDir folder;
        const QString epub = folder.filePath(QStringLiteral("book.epub"));
        Run run = tool({ QStringLiteral("make-epub"), epub, QStringLiteral("Fish & Chips"), QStringLiteral("An Author"),
                         QStringLiteral("en"), QStringLiteral("One."), QStringLiteral("Two.") });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));
        // An EPUB starts with its mimetype, stored.
        const QByteArray data = readAll(epub);
        QVERIFY(data.startsWith("PK\x03\x04"));
        QCOMPARE(data.mid(8, 2), QByteArray("\0\0", 2));
        QCOMPARE(data.mid(30, 8), QByteArray("mimetype"));
        int error = 0;
        zip_t *zip = zip_open(QFile::encodeName(epub).constData(), ZIP_RDONLY, &error);
        QVERIFY(zip);
        QCOMPARE(zip_get_num_entries(zip, 0), zip_int64_t(5));
        zip_discard(zip);

        const QString pdf = folder.filePath(QStringLiteral("book.pdf"));
        run = tool({ QStringLiteral("make-pdf"), pdf, QStringLiteral("Hello") });
        QVERIFY2(run.exitCode == 0, qPrintable(run.output));
        const QByteArray written = readAll(pdf);
        QVERIFY(written.startsWith("%PDF-1.4\n"));
        QVERIFY(written.contains("(Hello) Tj"));
        QVERIFY(written.endsWith("%%EOF\n"));
    }
};

QTEST_GUILESS_MAIN(TestToolTest)
#include "test_tool_test.moc"
