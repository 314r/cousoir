#include <QtTest>

#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "qr_code.h"
#include "test_support.h"

namespace {

struct Fixture
{
    QByteArray text;
    QStringList rows;
};

Fixture fixture(const QString &name)
{
    const QList<QByteArray> lines = readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/qr/") + name).split('\n');
    Fixture result;
    result.text = lines.value(0);
    for (qsizetype i = 1; i < lines.size(); ++i) {
        if (!lines.at(i).isEmpty())
            result.rows.append(QString::fromLatin1(lines.at(i)));
    }
    return result;
}

// The level and mask of a code, from the format information beside its top
// left finder.
std::pair<int, int> format(const QStringList &rows)
{
    const auto dark = [&](int x, int y) { return rows.at(y).at(x) == u'#'; };
    int bits = 0;
    const auto set = [&bits](int i, bool on) { bits |= int(on) << i; };
    for (int i = 0; i <= 5; ++i)
        set(i, dark(8, i));
    set(6, dark(8, 7));
    set(7, dark(8, 8));
    set(8, dark(7, 8));
    for (int i = 9; i < 15; ++i)
        set(i, dark(14 - i, 8));
    const int data = (bits ^ 0x5412) >> 10;
    return { data >> 3, data & 7 };
}

} // namespace

class QrCodeTest : public QObject
{
    Q_OBJECT

private slots:
    void matchesLibqrencode_data();
    void matchesLibqrencode();
    void scansWithZbar();
    void refusesTooLongTexts();
    void drawsSvg();
};

void QrCodeTest::matchesLibqrencode_data()
{
    QTest::addColumn<QString>("name");
    // tools/make_qr_fixtures.sh made them: versions 2, 3, 5, 8 and 13, which
    // has a 16-bit count and more blocks.
    for (int i = 1; i <= 5; ++i)
        QTest::newRow(qPrintable(QString::number(i))) << QStringLiteral("%1.txt").arg(i);
}

void QrCodeTest::matchesLibqrencode()
{
    QFETCH(QString, name);
    const Fixture expected = fixture(name);
    QVERIFY(!expected.text.isEmpty());
    const auto [level, mask] = format(expected.rows);
    QCOMPARE(level, 0);

    // The same mask, for the same modules.
    const QrCode::Matrix matrix = QrCode::encode(expected.text, mask);
    QCOMPARE(matrix.size, int(expected.rows.size()));
    QCOMPARE(matrix.version, (matrix.size - 17) / 4);
    QStringList rows;
    for (int y = 0; y < matrix.size; ++y) {
        QString row;
        for (int x = 0; x < matrix.size; ++x)
            row += matrix.dark(x, y) ? u'#' : u'.';
        rows.append(row);
    }
    QCOMPARE(rows, expected.rows);

    // A mask of its own choice changes only the masked modules.
    const QrCode::Matrix chosen = QrCode::encode(expected.text);
    QCOMPARE(chosen.size, matrix.size);
    QVERIFY(chosen.mask >= 0 && chosen.mask < 8);
}

void QrCodeTest::scansWithZbar()
{
    // What a phone's camera reads, as zbarimg reads it, where it is
    // installed.
    const QString zbarimg = QStandardPaths::findExecutable(QStringLiteral("zbarimg"));
    if (zbarimg.isEmpty())
        QSKIP("zbarimg is not installed");
    QTemporaryDir folder;
    for (int i = 1; i <= 5; ++i) {
        const QByteArray text = fixture(QStringLiteral("%1.txt").arg(i)).text;
        const QrCode::Matrix matrix = QrCode::encode(text);
        constexpr int scale = 4;
        constexpr int border = 4;
        QImage image((matrix.size + 2 * border) * scale, (matrix.size + 2 * border) * scale, QImage::Format_Grayscale8);
        image.fill(255);
        for (int y = 0; y < matrix.size; ++y) {
            for (int x = 0; x < matrix.size; ++x) {
                if (!matrix.dark(x, y))
                    continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx)
                        image.setPixel((x + border) * scale + dx, (y + border) * scale + dy, qRgb(0, 0, 0));
                }
            }
        }
        const QString file = folder.filePath(QStringLiteral("%1.png").arg(i));
        QVERIFY(image.save(file));
        QProcess zbar;
        zbar.start(zbarimg, { QStringLiteral("--raw"), QStringLiteral("-q"), QStringLiteral("-Sbinary"), file });
        QVERIFY(zbar.waitForFinished(20000));
        QCOMPARE(zbar.readAllStandardOutput().trimmed(), text);
    }
}

void QrCodeTest::refusesTooLongTexts()
{
    QCOMPARE(QrCode::encode(QByteArray(3000, 'a')).size, 0);
    // Version 40 at level M holds 2331 bytes.
    QCOMPARE(QrCode::encode(QByteArray(2331, 'a')).version, 40);
}

void QrCodeTest::drawsSvg()
{
    const QrCode::Matrix matrix = QrCode::encode("http://192.168.1.20:8090/");
    const QByteArray svg = QrCode::svg(matrix);
    // The code and four light modules on each side.
    QVERIFY(svg.contains("viewBox=\"0 0 33 33\""));
    // The top left finder's corner.
    QVERIFY(svg.contains("M4,4h1v1h-1z"));
}

QTEST_GUILESS_MAIN(QrCodeTest)

#include "qr_code_test.moc"
