#include <QtTest>

#include <QDir>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include "mobi_metadata.h"
#include "test_support.h"

namespace {

QString mobiData(const QString &name = {})
{
    return QStringLiteral(COUSOIR_TEST_DATA_DIR "/mobi/") + name;
}

// tools/oracle/mobi_oracle.py fixed Calibre's random ASIN so.
const QString fixedUuid = QStringLiteral("00000000-0000-4000-8000-000000000001");

QStringList strings(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray())
        result.append(item.toString());
    return result;
}

std::pair<MobiMetadata::Metadata, MobiMetadata::Options> readCase(const QJsonObject &values)
{
    MobiMetadata::Metadata metadata;
    metadata.title = values.value(QLatin1String("title")).toString();
    metadata.authors = strings(values.value(QLatin1String("authors")));
    metadata.authorSort = values.value(QLatin1String("author_sort")).toString();
    metadata.publisher = values.value(QLatin1String("publisher")).toString();
    metadata.comments = values.value(QLatin1String("comments")).toString();
    metadata.isbn = values.value(QLatin1String("isbn")).toString();
    metadata.tags = strings(values.value(QLatin1String("tags")));
    metadata.pubdate = QDateTime::fromString(values.value(QLatin1String("pubdate")).toString(), Qt::ISODate);
    metadata.uuid = values.value(QLatin1String("uuid")).toString();
    metadata.languages = strings(values.value(QLatin1String("languages")));
    if (values.contains(QLatin1String("cover")))
        metadata.cover = readAll(mobiData(values.value(QLatin1String("cover")).toString()));
    const QJsonObject saved = values.value(QLatin1String("options")).toObject();
    MobiMetadata::Options options;
    options.preferAuthorSort = saved.value(QLatin1String("prefer_author_sort")).toBool();
    options.personalDoc = saved.value(QLatin1String("personal_doc")).toString();
    options.shareNotSync = saved.value(QLatin1String("share_not_sync")).toBool();
    return { metadata, options };
}

// A MOBI file's records.
QList<QByteArray> records(const QByteArray &book)
{
    const int count = qFromBigEndian<quint16>(book.constData() + 76);
    QList<QByteArray> result;
    for (int i = 0; i < count; ++i) {
        const qsizetype start = qFromBigEndian<quint32>(book.constData() + 78 + 8 * i);
        const qsizetype end = i + 1 < count ? qFromBigEndian<quint32>(book.constData() + 78 + 8 * (i + 1)) : book.size();
        result.append(book.mid(start, end - start));
    }
    return result;
}

} // namespace

class MobiMetadataTest : public QObject
{
    Q_OBJECT

private slots:
    void writesAsCalibre_data();
    void writesAsCalibre();
    void namesThumbnailsAndPages();
    void refusesOtherFiles();
};

void MobiMetadataTest::writesAsCalibre_data()
{
    QTest::addColumn<QString>("book");
    QTest::addColumn<QString>("caseName");
    const QJsonObject cases = QJsonDocument::fromJson(readAll(mobiData(QStringLiteral("cases.json")))).object();
    for (const QString &book : QDir(mobiData()).entryList({ QStringLiteral("*.mobi"), QStringLiteral("*.azw3") }, QDir::Files)) {
        for (const QString &name : cases.keys())
            QTest::newRow(qPrintable(book + u' ' + name)) << book << name;
    }
}

void MobiMetadataTest::writesAsCalibre()
{
    QFETCH(QString, book);
    QFETCH(QString, caseName);
    const QJsonObject cases = QJsonDocument::fromJson(readAll(mobiData(QStringLiteral("cases.json")))).object();
    const auto [metadata, options] = readCase(cases.value(caseName).toObject());
    QByteArray written = readAll(mobiData(book));
    QVERIFY(!written.isEmpty());
    QString error;
    const bool ok = MobiMetadata::update(&written, metadata, options, &error, fixedUuid);
    const QFileInfo info(book);
    const QByteArray expected = readAll(mobiData(QStringLiteral("calibre/%1-%2.%3").arg(info.completeBaseName(), caseName, info.suffix())));
    QVERIFY(!expected.isEmpty());
    if (expected.startsWith("error:")) {
        QVERIFY2(!ok, qPrintable(QString::fromUtf8(expected)));
        return;
    }
    QVERIFY2(ok, qPrintable(error));
    if (book == QLatin1String("bare.mobi")) {
        // Calibre loses the text of a book without an EXTH header (see
        // tools/oracle/mobi_oracle.py): its first record, and the text kept.
        const QList<QByteArray> ours = records(written);
        QCOMPARE(ours.size(), 2);
        QCOMPARE(ours.first(), records(expected).first());
        QCOMPARE(ours.at(1), records(readAll(mobiData(book))).at(1));
        return;
    }
    if (!metadata.cover.isEmpty()) {
        // The images are JPEGs of Qt's own: the rest is Calibre's, and each
        // image record stays its size and is an image.
        const QList<QByteArray> ours = records(written);
        const QList<QByteArray> theirs = records(expected);
        QCOMPARE(ours.size(), theirs.size());
        for (int i = 0; i < ours.size(); ++i) {
            if (ours.at(i) == theirs.at(i))
                continue;
            QCOMPARE(ours.at(i).size(), theirs.at(i).size());
            QVERIFY2(theirs.at(i).startsWith("\xff\xd8"), qPrintable(QStringLiteral("record %1 differs").arg(i)));
            QVERIFY(!QImage::fromData(ours.at(i)).isNull());
        }
        QCOMPARE(written.left(78 + 8 * ours.size()), expected.left(78 + 8 * ours.size()));
        return;
    }
    if (written != expected) {
        // Where they part, for the failure.
        qsizetype at = 0;
        while (at < written.size() && at < expected.size() && written.at(at) == expected.at(at))
            ++at;
        QFAIL(qPrintable(QStringLiteral("differs from byte %1 of %2/%3: %4 for %5")
                             .arg(at)
                             .arg(written.size())
                             .arg(expected.size())
                             .arg(QString::fromLatin1(written.mid(at, 24).toHex()), QString::fromLatin1(expected.mid(at, 24).toHex()))));
    }
}

void MobiMetadataTest::namesThumbnailsAndPages()
{
    const QJsonObject names = QJsonDocument::fromJson(readAll(mobiData(QStringLiteral("calibre/thumbnails.json")))).object();
    QVERIFY(!names.isEmpty());
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        if (it.key().endsWith(QLatin1String(".apnx")))
            continue;
        const QByteArray book = readAll(mobiData(QStringLiteral("calibre/") + it.key()));
        QCOMPARE(MobiMetadata::thumbnailName(book), it.value().toString());
        const QString apnxFile = mobiData(QStringLiteral("calibre/") + it.key() + QStringLiteral(".apnx"));
        QString error;
        const QByteArray apnx = MobiMetadata::apnx(book, QStringLiteral("00000000"), &error);
        if (QFileInfo::exists(apnxFile))
            QCOMPARE(apnx, readAll(apnxFile));
        else
            QVERIFY(apnx.isEmpty());
    }
}

void MobiMetadataTest::refusesOtherFiles()
{
    QByteArray notMobi("%PDF-1.4 not a MOBI file at all, as long as a header would be, and longer still.");
    QString error;
    QVERIFY(!MobiMetadata::update(&notMobi, {}, {}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(MobiMetadata::thumbnailName(notMobi).isEmpty());
    QVERIFY(MobiMetadata::apnx(notMobi, QStringLiteral("00000000"), &error).isEmpty());
}

QTEST_GUILESS_MAIN(MobiMetadataTest)

#include "mobi_metadata_test.moc"
