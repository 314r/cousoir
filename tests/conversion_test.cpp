#include <QtTest>

#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <zip.h>

#include "calibre_db.h"
#include "calibre_job.h"
#include "commit_engine.h"
#include "conversion.h"
#include "sqlite_db.h"
#include "test_support.h"

namespace {

// pickle_binary_string() of calibre/utils/__init__.py.
QByteArray pickled(const QByteArray &raw)
{
    QByteArray data("\x80\x02T", 3);
    const quint32 size = quint32(raw.size());
    for (int shift = 0; shift < 32; shift += 8)
        data.append(char((size >> shift) & 0xff));
    return data + raw + '.';
}

// The text of every HTML file of an EPUB.
QByteArray epubText(const QString &path)
{
    int error = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &error);
    if (!archive)
        return {};
    QByteArray text;
    for (zip_int64_t index = 0; index < zip_get_num_entries(archive, 0); ++index) {
        const QString name = QString::fromUtf8(zip_get_name(archive, index, 0));
        zip_stat_t stat;
        if (!name.contains(QStringLiteral("htm")) || zip_stat_index(archive, index, 0, &stat) != 0)
            continue;
        zip_file_t *file = zip_fopen_index(archive, index, 0);
        QByteArray data(qsizetype(stat.size), Qt::Uninitialized);
        if (file && zip_fread(file, data.data(), stat.size) == zip_int64_t(stat.size))
            text += data;
        if (file)
            zip_fclose(file);
    }
    zip_close(archive);
    return text;
}

QStringList entries(const QString &path)
{
    int error = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &error);
    QStringList names;
    for (zip_int64_t index = 0; archive && index < zip_get_num_entries(archive, 0); ++index)
        names.append(QString::fromUtf8(zip_get_name(archive, index, 0)));
    if (archive)
        zip_close(archive);
    return names;
}

} // namespace

class ConversionTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_snapshotFolder;
    LibraryData m_data;

private slots:
    void initTestCase()
    {
        CalibreConfig config;
        config.interfaceLanguage = QStringLiteral("fr");
        QString error;
        QVERIFY2(CalibreDb::loadLibrary(fixtureLibrary(), m_snapshotFolder.filePath(QStringLiteral("snapshot.db")), config,
                                        &m_data, &error),
                 qPrintable(error));
    }

    void offersCalibresInputFormats()
    {
        QCOMPARE(Conversion::outputFormats(), QStringList({ QStringLiteral("KEPUB"), QStringLiteral("AZW3") }));
        QCOMPARE(Conversion::inputFormats(*m_data.book(2)),
                 QStringList({ QStringLiteral("AZW3"), QStringLiteral("EPUB") }));
        QCOMPARE(Conversion::inputFormats(*m_data.book(5)), QStringList({ QStringLiteral("PDF") }));
        // No file at all.
        QVERIFY(Conversion::inputFormats(*m_data.book(6)).isEmpty());
        Book other = *m_data.book(2);
        other.formats = { { QStringLiteral("ORIGINAL_EPUB"), {}, 0, {} }, { QStringLiteral("XYZ"), {}, 0, {} } };
        QVERIFY(Conversion::inputFormats(other).isEmpty());
    }

    // The OPF of every book as create_opf_file() writes it, by
    // tools/oracle/conversion_oracle.py.
    void writesCalibresOpf()
    {
        const QJsonObject oracle =
            QJsonDocument::fromJson(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/oracle/conversion.json"))).object();
        const QJsonObject opfs = oracle.value(QLatin1String("opfs")).toObject();
        QVERIFY(!opfs.isEmpty());
        for (auto it = opfs.constBegin(); it != opfs.constEnd(); ++it) {
            const Book *book = m_data.book(it.key().toInt());
            QVERIFY(book);
            QTemporaryDir folder;
            QString error;
            QVERIFY2(Conversion::prepare(m_data, *book, QStringLiteral("KEPUB"), QStringLiteral("fr"), folder.path(), &error),
                     qPrintable(error));
            QByteArray opf = readAll(folder.filePath(QStringLiteral("book.opf")));
            opf.replace("@CALIBRE_PRODUCER@", oracle.value(QLatin1String("producer")).toString().toUtf8());
            QCOMPARE(QString::fromUtf8(opf), it.value().toString());
        }
    }

    void writesJobs()
    {
        const Book book = *m_data.book(2);
        QTemporaryDir folder;
        QString error;
        QVERIFY2(Conversion::prepare(m_data, book, QStringLiteral("AZW3"), QStringLiteral("fr"), folder.path(), &error),
                 qPrintable(error));
        const QJsonObject job = QJsonDocument::fromJson(readAll(folder.filePath(QStringLiteral("job.json")))).object();
        QCOMPARE(job.value(QLatin1String("book")).toInt(), 2);
        QCOMPARE(job.value(QLatin1String("format")).toString(), QStringLiteral("azw3"));
        const QJsonObject formats = job.value(QLatin1String("formats")).toObject();
        QCOMPARE(formats.keys(), QStringList({ QStringLiteral("AZW3"), QStringLiteral("EPUB") }));
        QVERIFY(QFileInfo::exists(formats.value(QLatin1String("EPUB")).toString()));
        QVERIFY(!job.contains(QLatin1String("options")));
        QCOMPARE(job.value(QLatin1String("cover")).toString(), QStringLiteral("cover.jpeg"));
        QCOMPARE(readAll(folder.filePath(QStringLiteral("cover.jpeg"))), readAll(book.coverFile));
        QVERIFY(readAll(folder.filePath(QStringLiteral("convert.py"))).contains("gui_convert_override"));
        QCOMPARE(Conversion::arguments(folder.path()),
                 QStringList({ QStringLiteral("-e"), folder.filePath(QStringLiteral("convert.py")), QStringLiteral("--"),
                               folder.path() }));

        // The settings saved by the book's last conversion go as stored; a
        // book without cover has none; a missing file is where Calibre
        // expects it.
        Book other = *m_data.book(5);
        other.conversionOptions = pickled("json:{\"output_profile\": \"kobo\"}");
        other.formats.first().file.clear();
        QTemporaryDir second;
        QVERIFY2(Conversion::prepare(m_data, other, QStringLiteral("KEPUB"), QStringLiteral("fr"), second.path(), &error),
                 qPrintable(error));
        const QJsonObject secondJob = QJsonDocument::fromJson(readAll(second.filePath(QStringLiteral("job.json")))).object();
        QCOMPARE(QByteArray::fromBase64(secondJob.value(QLatin1String("options")).toString().toLatin1()),
                 other.conversionOptions);
        QVERIFY(!secondJob.contains(QLatin1String("cover")));
        QVERIFY(!QFileInfo::exists(second.filePath(QStringLiteral("cover.jpeg"))));
        QCOMPARE(secondJob.value(QLatin1String("formats")).toObject().value(QLatin1String("PDF")).toString(),
                 QDir(QDir(m_data.libraryPath).filePath(other.path)).filePath(other.formats.first().name + u".pdf"));
    }

    void readsOutcomes()
    {
        QCOMPARE(CalibreJob::progress("{\"progress\": 0.34}\n"), 0.34);
        QCOMPARE(CalibreJob::progress("{\"progress\": 1.0}"), 1.0);
        QCOMPARE(CalibreJob::progress("Using calibre Qt style: True\n"), -1.0);
        QCOMPARE(CalibreJob::progress("{\"other\": 1}"), -1.0);

        QTemporaryDir folder;
        const QString result = folder.filePath(QStringLiteral("result.json"));
        QVERIFY(writeAll(result, "{\"input\": \"epub\", \"file\": \"output.kepub\", \"save_original\": true, "
                                 "\"options\": \"" + pickled("json:{}").toBase64() + "\"}"));
        Conversion::Outcome outcome = Conversion::outcome(folder.path(), 0);
        QVERIFY(outcome.ok);
        QCOMPARE(outcome.inputFormat, QStringLiteral("epub"));
        QCOMPARE(outcome.file, folder.filePath(QStringLiteral("output.kepub")));
        QVERIFY(outcome.saveOriginal);
        QCOMPARE(outcome.options, pickled("json:{}"));

        // Each refusal of the job says why.
        const QList<QPair<QByteArray, QString>> refusals{
            { "{\"error\": \"input\", \"detail\": \"\"}", QStringLiteral("none of the book's formats") },
            { "{\"error\": \"missing\", \"detail\": \"EPUB\"}", QStringLiteral("EPUB file is missing") },
            { "{\"error\": \"settings\", \"detail\": \"Invalid XPath\"}", QStringLiteral("Invalid XPath") },
            { "{\"error\": \"empty\", \"detail\": \"\"}", QStringLiteral("empty file") },
            { "{\"error\": \"drm\", \"detail\": \"A Title\"}", QStringLiteral("locked by DRM") },
            { "{\"error\": \"failed\", \"detail\": \"DRMError\"}", QStringLiteral("DRMError") },
        };
        for (const auto &[json, expected] : refusals) {
            QVERIFY(writeAll(result, json));
            outcome = Conversion::outcome(folder.path(), 1);
            QVERIFY(!outcome.ok);
            QVERIFY2(outcome.error.contains(expected), qPrintable(outcome.error));
        }

        // Calibre gone before its result: the end of its log.
        QFile::remove(result);
        QVERIFY(writeAll(folder.filePath(QStringLiteral("log.txt")), "Converting...\nMemoryError\n"));
        outcome = Conversion::outcome(folder.path(), -1);
        QVERIFY(!outcome.ok);
        QVERIFY2(outcome.error.contains(QStringLiteral("MemoryError")), qPrintable(outcome.error));
    }

    // book_converted(): the file added, after ORIGINAL_<format> when Calibre
    // keeps it, and the settings for the next conversion; the book is
    // touched, not dirtied.
    void addsConversions()
    {
        QTemporaryDir root;
        const QString library = root.filePath(QStringLiteral("library"));
        QVERIFY(copyTree(fixtureLibrary(), library));
        CalibreConfig config;
        config.interfaceLanguage = QStringLiteral("fr");
        LibraryData data;
        QString error;
        QVERIFY2(CalibreDb::loadLibrary(library, root.filePath(QStringLiteral("snapshot.db")), config, &data, &error),
                 qPrintable(error));
        const QString converted = root.filePath(QStringLiteral("output.azw3"));
        QVERIFY(writeAll(converted, "BOOKMOBI converted"));
        Conversion::Outcome outcome;
        outcome.ok = true;
        outcome.inputFormat = QStringLiteral("azw3");
        outcome.file = converted;
        outcome.saveOriginal = true;
        outcome.options = pickled("json:{\n  \"gui_preferred_input_format\": \"azw3\"\n}");
        const Book &book = *data.book(2);
        const QByteArray original = readAll(book.formats.first().file);

        CommitEngine::Request request;
        Conversion::addTo(&request, book, QStringLiteral("azw3"), outcome);
        QCOMPARE(request.newFormats.size(), 2);
        QCOMPARE(request.newFormats.at(0).format, QStringLiteral("ORIGINAL_AZW3"));
        QCOMPARE(request.newFormats.at(0).file, book.formats.first().file);
        QCOMPARE(request.newFormats.at(1).format, QStringLiteral("AZW3"));
        QCOMPARE(request.conversionOptions.size(), 1);

        request.libraryPath = library;
        request.cacheFolder = root.filePath(QStringLiteral("cache"));
        request.config = config;
        request.producer = QStringLiteral("Cousoir (test)");
        request.producerId = QStringLiteral("cousoir");
        request.now = QDateTime(QDate(2026, 10, 19), QTime(12, 0), QTimeZone::UTC);
        const CommitEngine::Result result = CommitEngine::commit(request);
        QVERIFY2(result.ok, qPrintable(result.error));

        const QDir folder(QDir(library).filePath(book.path));
        QCOMPARE(readAll(folder.filePath(QStringLiteral("The Black Company - Glen Cook.original_azw3"))), original);
        QCOMPARE(readAll(folder.filePath(QStringLiteral("The Black Company - Glen Cook.azw3"))),
                 QByteArray("BOOKMOBI converted"));
        SqliteDb db;
        QVERIFY(db.open(QDir(library).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
        QCOMPARE(db.value(QStringLiteral("SELECT group_concat(format, ',') FROM (SELECT format FROM data WHERE book=2 "
                                         "ORDER BY format)")).toString(),
                 QStringLiteral("AZW3,EPUB,ORIGINAL_AZW3"));
        QCOMPARE(db.value(QStringLiteral("SELECT data FROM conversion_options WHERE book=2 AND format='PIPE'")).toByteArray(),
                 outcome.options);
        QCOMPARE(db.value(QStringLiteral("SELECT last_modified FROM books WHERE id=2")).toString(),
                 QStringLiteral("2026-10-19 12:00:00+00:00"));
        QCOMPARE(db.value(QStringLiteral("SELECT count(*) FROM metadata_dirtied")).toInt(), 0);
    }

    // Calibre itself, when it is installed, with its default settings.
    void convertsWithCalibre()
    {
        if (Conversion::program().isEmpty())
            QSKIP("Calibre is not installed");
        QTemporaryDir config;
        qputenv("CALIBRE_CONFIG_DIRECTORY", QFile::encodeName(config.path()));
        QTemporaryDir folder;
        QString error;
        QVERIFY2(Conversion::prepare(m_data, *m_data.book(1), QStringLiteral("KEPUB"), QStringLiteral("fr"),
                                     folder.path(), &error),
                 qPrintable(error));
        QList<double> progress;
        const Conversion::Outcome outcome
            = Conversion::run(folder.path(), QStringLiteral("fr"), [&progress](double fraction) { progress.append(fraction); });
        qunsetenv("CALIBRE_CONFIG_DIRECTORY");
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(outcome.inputFormat, QStringLiteral("epub"));
        QVERIFY(!outcome.saveOriginal);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last(), 1.0);
        QVERIFY(outcome.options.startsWith(QByteArray("\x80\x02T", 3)));
        QVERIFY(outcome.options.contains("json:{"));
        QVERIFY(outcome.options.contains("\"gui_preferred_input_format\": \"epub\""));
        // What makes a KEPUB.
        QVERIFY(entries(outcome.file).contains(QStringLiteral("kobo.js")));
        const QByteArray text = epubText(outcome.file);
        QVERIFY(text.contains("koboSpan"));
        QVERIFY(text.contains("book-columns"));
        QVERIFY(text.contains("L'Écume des jours") || text.contains("L’Écume des jours"));
        // Calibre's temporary files stayed in the job's folder.
        QVERIFY(QFileInfo(folder.filePath(QStringLiteral("tmp"))).isDir());
    }
};

QTEST_GUILESS_MAIN(ConversionTest)
#include "conversion_test.moc"
