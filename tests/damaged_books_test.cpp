#include <QtTest>

#include <QDirIterator>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include "book_import.h"
#include "calibre_config.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "epub_package.h"
#include "kepub.h"
#include "opf_reader.h"
#include "pdf_reader.h"
#include "test_support.h"
#include "text_book_epub.h"
#include "text_book_metadata.h"
#include "xml_tree.h"

#include <array>

// Books damaged every which way, as files from anywhere may be, through each
// reader Cousoir has of its own: EPUB and KEPUB as the reader and the import
// read them, the package documents, the pages made for a Kobo, the places of
// highlights, a PDF's metadata, FictionBooks and plain text. Read or
// refused, never a crash: run them under AddressSanitizer too (make
// sanitize). What Cousoir makes of a damaged book still opens.
//
// COUSOIR_FUZZ_ROUNDS and COUSOIR_FUZZ_SEED make longer, other runs, as for
// the Kindle books of epub_book_tests.
class DamagedBooksTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void survivesDamagedEpubs();
    void survivesDamagedPackageDocuments();
    void survivesDamagedPagesForAKobo();
    void survivesDamagedPlaces();
    void survivesDamagedPdfs();
    void survivesDamagedFictionBooksAndText();

private:
    QTemporaryDir m_dir;
};

namespace {

int rounds(int byDefault)
{
    const int asked = qEnvironmentVariableIntValue("COUSOIR_FUZZ_ROUNDS");
    return asked > 0 ? asked : byDefault;
}

QStringList files(const QString &folder, const QStringList &patterns)
{
    QStringList found;
    QDirIterator it(folder, patterns, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        found.append(it.next());
    found.sort();
    return found;
}

// The damage: a seeded generator, the same run after run unless
// COUSOIR_FUZZ_SEED says otherwise.
class Damage
{
public:
    explicit Damage(quint32 salt)
        : m_random(quint32(qEnvironmentVariableIntValue("COUSOIR_FUZZ_SEED")) + salt)
    {
    }

    int below(int bound) { return bound > 0 ? int(m_random.bounded(bound)) : 0; }

    // Bytes changed, the end cut, a part gone or repeated.
    QByteArray bytes(QByteArray data)
    {
        if (data.isEmpty())
            return data;
        switch (below(4)) {
        case 0:
            for (int n = 0, count = 1 + below(40); n < count; ++n)
                data[below(int(data.size()))] = char(below(256));
            break;
        case 1:
            data.truncate(below(int(data.size())));
            break;
        case 2:
            data.remove(below(int(data.size())), 1 + below(400));
            break;
        default:
        {
            const int at = below(int(data.size()));
            data.insert(below(int(data.size())), data.mid(at, 1 + below(400)));
        }
        }
        return data;
    }

    // Damage that keeps to markup's shapes, which a parser goes further
    // into than it does with bytes changed at random: hostile snippets,
    // elements renamed, numbers and addresses at their extremes, parts gone
    // or repeated, the end cut.
    QByteArray markup(QByteArray data)
    {
        for (int n = 0, count = 1 + below(3); n < count && !data.isEmpty(); ++n) {
            switch (below(7)) {
            case 0:
                data.insert(below(int(data.size()) + 1), snippet());
                break;
            case 1:
                replaceAfter(&data, "<", nameLike(), [](char c) { return isNameChar(c); });
                break;
            case 2:
                replaceAfter(&data, "", numberLike(), [](char c) { return c >= '0' && c <= '9'; }, true);
                break;
            case 3:
                replaceAfter(&data, "=\"", valueLike(), [](char c) { return c != '"'; });
                break;
            default:
                data = bytes(data);
            }
        }
        return data;
    }

    // A file of an EPUB damaged: the package, the contents, a page, or
    // the files themselves, gone, renamed or added under hostile names.
    void package(EpubPackage::Zip *zip)
    {
        if (zip->names.isEmpty())
            return;
        const QString opf = EpubPackage::packageName(*zip);
        QStringList markupFiles;
        for (const QString &name : std::as_const(zip->names)) {
            if (name.endsWith(QLatin1String(".xhtml")) || name.endsWith(QLatin1String(".html"))
                || name.endsWith(QLatin1String(".htm")) || name.endsWith(QLatin1String(".xml"))
                || name.endsWith(QLatin1String(".opf")) || name.endsWith(QLatin1String(".ncx")))
                markupFiles.append(name);
        }
        for (int n = 0, count = 1 + below(3); n < count; ++n) {
            const int kind = below(10);
            if (kind < 3 && !opf.isEmpty()) {
                zip->data[opf] = markup(zip->data.value(opf));
            } else if (kind < 7 && !markupFiles.isEmpty()) {
                const QString name = markupFiles.at(below(int(markupFiles.size())));
                zip->data[name] = markup(zip->data.value(name));
            } else if (kind == 7) {
                const QString name = zip->names.at(below(int(zip->names.size())));
                zip->names.removeAll(name);
                zip->data.remove(name);
            } else if (kind == 8) {
                const int at = below(int(zip->names.size()));
                const QString from = zip->names.at(at);
                const QString to = pick<QString>({ QStringLiteral("../outside.xhtml"), QStringLiteral("a//b.xhtml"),
                                                   QStringLiteral("META-INF/container.xml"), QStringLiteral("/root.opf"),
                                                   from.toUpper(), from + QStringLiteral(" ") });
                if (zip->names.contains(to))
                    continue;
                zip->names[at] = to;
                zip->data.insert(to, zip->data.take(from));
            } else {
                const QString name = pick<QString>({ QStringLiteral("../../escape.xhtml"), QStringLiteral("x/../../y"),
                                                     QStringLiteral("é́.xhtml"), QStringLiteral("dir/") });
                if (!zip->names.contains(name)) {
                    zip->names.append(name);
                    zip->data.insert(name, snippet());
                }
            }
        }
    }

    template <typename T>
    T pick(std::initializer_list<T> choices)
    {
        return *(choices.begin() + below(int(choices.size())));
    }

private:
    static bool isNameChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '-'
            || c == '_' || c == '.';
    }

    // Replaces by `with` what follows a random occurrence of `marker` while
    // `keep` holds; with `anyRun`, a run of what `keep` holds anywhere.
    template <typename Keep>
    void replaceAfter(QByteArray *data, const char *marker, const QByteArray &with, Keep keep, bool anyRun = false)
    {
        qsizetype start = -1;
        for (int tries = 0; tries < 20 && start < 0; ++tries) {
            const qsizetype from = below(int(data->size()));
            if (anyRun) {
                qsizetype at = from;
                while (at < data->size() && !keep(data->at(at)))
                    ++at;
                if (at < data->size())
                    start = at;
            } else {
                const qsizetype at = data->indexOf(marker, from);
                if (at >= 0)
                    start = at + qsizetype(qstrlen(marker));
            }
        }
        if (start < 0)
            return;
        qsizetype end = start;
        while (end < data->size() && keep(data->at(end)))
            ++end;
        data->replace(start, end - start, with);
    }

    QByteArray snippet()
    {
        switch (below(26)) {
        case 0: return "<";
        case 1: return ">";
        case 2: return "&";
        case 3: return "&#0;";
        case 4: return "&#x110000;";
        case 5: return "&#xD800;";
        case 6: return "&nbsp;&unknown;";
        case 7: return "]]>";
        case 8: return "<![CDATA[";
        case 9: return "<!--";
        case 10: return "<?xml version=\"1.0\" encoding=\"x-unknown\"?>";
        case 11: return "<!DOCTYPE x [<!ENTITY a \"&a;&a;\"><!ENTITY b \"&a;&a;&a;&a;\">]>&b;";
        case 12: return "\"'";
        case 13: return "</";
        case 14: return "/>";
        case 15: return "<a href=\"";
        case 16: return " xmlns=\"\" xmlns:x=\"urn:x\" x:y=\"\"";
        case 17: return QByteArray("<div>").repeated(1 + below(3000));
        case 18: return QByteArray("<span><b>").repeated(1 + below(600));
        case 19: return "\xff\xfe\xc3";
        case 20: return QByteArray(1, '\0');
        case 21: return "<img src=\"data:image/png;base64,AAAA\"/>";
        case 22: return "<a href=\"#\" id=\"\"/><a id=\"dup\"/><a id=\"dup\"/>";
        case 23: return QByteArray(1 + below(200000), 'x');
        case 24: return "<navPoint><content src=\"\"/></navPoint>";
        default: return "<itemref idref=\"missing\"/><item id=\"\" href=\"\" media-type=\"\"/>";
        }
    }

    QByteArray nameLike()
    {
        return pick<QByteArray>({ "p", "div", "html", "body", "head", "item", "itemref", "spine", "manifest",
                                  "metadata", "navPoint", "navMap", "nav", "ol", "li", "a", "meta", "dc:title",
                                  "dc:creator", "rootfile", "svg", "image", "x:y", "", "1" });
    }

    QByteArray numberLike()
    {
        return pick<QByteArray>({ "-1", "0", "99999999999999999999", "1e308", "NaN", "4294967296", "2147483648" });
    }

    QByteArray valueLike()
    {
        return pick<QByteArray>({ "", "#", "../../../etc/passwd", "x.xhtml#", "%", "%zz", "data:,", "http://a",
                                  "/abs", "javascript:x", QByteArray(100000, 'v'), "a\nb" });
    }

    QRandomGenerator m_random;
};

// Reads what a reader would of a book: each page, parsed, its text, the
// places of its elements and where they lead back, the table of contents.
void readAsTheReaderDoes(const EpubBook &book, Damage *damage)
{
    for (const EpubBook::TocEntry &entry : book.toc())
        book.spineIndex(entry.name);
    for (const EpubBook::SpineItem &item : book.spine()) {
        const QByteArray page = book.read(item.name);
        book.mediaType(item.name);
        const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(page);
        if (!root)
            continue;
        const EpubCfi::Document document(*root);
        const EpubCfi::PageText text = EpubCfi::PageText::of(*root, true);
        if (!text.starts.isEmpty()) {
            const EpubCfi::Point start = text.starts.at(damage->below(int(text.starts.size())));
            const EpubCfi::Point end = text.ends.at(damage->below(int(text.ends.size())));
            const QString cfi = document.encode(*start.node, start.offset);
            document.decode(cfi);
            document.textBetween(start, end);
            text.indexOf(start);
        }
        document.element(QStringLiteral("dup"));
    }
}

} // namespace

void DamagedBooksTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    // No Calibre for the import's covers: thousands of books, each read
    // natively or refused.
    qputenv("COUSOIR_CALIBRE_DEBUG", "");
}

// EPUBs and KEPUBs damaged, in their archive or in their files: opened by
// the reader and read through, read by the import, made into a KEPUB. What
// the KEPUB of a book the reader opens is opens too.
void DamagedBooksTest::survivesDamagedEpubs()
{
    Damage damage(20261113);
    const CalibreConfig config = frenchConfig();
    QStringList books = files(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader"), { QStringLiteral("*.epub") })
        + files(QStringLiteral(COUSOIR_TEST_DATA_DIR "/import"), { QStringLiteral("*.epub") })
        + files(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub"), { QStringLiteral("*.epub") });
    QVERIFY(books.size() >= 6);
    const int perBook = rounds(30);
    int opened = 0;
    for (const QString &path : std::as_const(books)) {
        EpubPackage::Zip original;
        QString error;
        QVERIFY2(EpubPackage::readZip(path, &original, &error), qPrintable(path + u' ' + error));
        for (int round = 0; round < perBook; ++round) {
            const QString damaged = m_dir.filePath(round % 2 ? QStringLiteral("damaged.kepub") : QStringLiteral("damaged.epub"));
            if (round % 6 == 5) {
                // The archive itself.
                QVERIFY(writeAll(damaged, damage.bytes(readAll(path))));
            } else {
                EpubPackage::Zip zip = original;
                damage.package(&zip);
                QVERIFY2(EpubPackage::writeZip(damaged, zip, &error), qPrintable(error));
            }
            error.clear();
            const std::unique_ptr<EpubBook> book = EpubBook::open(damaged, &error);
            if (book) {
                ++opened;
                readAsTheReaderDoes(*book, &damage);
            } else {
                QVERIFY2(!error.isEmpty(), qPrintable(path));
            }

            ImportedMetadata metadata;
            error.clear();
            BookImport::readFileMetadata(damaged, config, &metadata, &error);

            const QString kepub = m_dir.filePath(QStringLiteral("made.kepub"));
            QFile::remove(kepub);
            error.clear();
            if (Kepub::kepubify(damaged, kepub, {}, &error) && book) {
                const std::unique_ptr<EpubBook> made = EpubBook::open(kepub, &error);
                QVERIFY2(made, qPrintable(QStringLiteral("%1, round %2: %3").arg(path).arg(round).arg(error)));
                readAsTheReaderDoes(*made, &damage);
            }
        }
    }
    // Most damages still leave a book to read.
    QVERIFY2(opened > books.size() * perBook / 3, qPrintable(QString::number(opened)));
}

// The package documents of the fixture library, and the EPUBs', damaged.
void DamagedBooksTest::survivesDamagedPackageDocuments()
{
    Damage damage(20261114);
    QList<QByteArray> documents;
    for (const QString &path : files(fixtureLibrary(), { QStringLiteral("metadata.opf") }))
        documents.append(readAll(path));
    for (const QString &path : files(QStringLiteral(COUSOIR_TEST_DATA_DIR "/import"), { QStringLiteral("*.epub") })) {
        EpubPackage::Zip zip;
        QString error;
        QVERIFY(EpubPackage::readZip(path, &zip, &error));
        const QString opf = EpubPackage::packageName(zip);
        if (!opf.isEmpty())
            documents.append(zip.data.value(opf));
    }
    QVERIFY(documents.size() >= 5);
    int read = 0;
    const int perDocument = rounds(60);
    for (const QByteArray &document : std::as_const(documents)) {
        for (int round = 0; round < perDocument; ++round) {
            OpfReader::Package package;
            QString error;
            if (OpfReader::read(damage.markup(document), &package, &error))
                ++read;
        }
    }
    QVERIFY2(read > 0, "no damaged package read at all");
}

// Pages made into a Kobo's, from pages damaged: XML, or nothing.
void DamagedBooksTest::survivesDamagedPagesForAKobo()
{
    Damage damage(20261115);
    QList<QByteArray> pages;
    for (const QString &path : files(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader"), { QStringLiteral("*.epub") })) {
        EpubPackage::Zip zip;
        QString error;
        QVERIFY(EpubPackage::readZip(path, &zip, &error));
        for (const QString &name : std::as_const(zip.names)) {
            if (name.endsWith(QLatin1String("html")))
                pages.append(zip.data.value(name));
        }
    }
    QVERIFY(pages.size() >= 5);
    const int perPage = rounds(30);
    for (const QByteArray &page : std::as_const(pages)) {
        for (int round = 0; round < perPage; ++round) {
            const std::optional<QByteArray> made = Kepub::kepubifyPage(damage.markup(page), QStringLiteral("kobo.js"));
            if (made)
                QVERIFY2(XmlTree::parse(*made), made->left(400).constData());
        }
    }
}

// Places in a page as an annotation may carry them, damaged: a node or
// none, never a crash.
void DamagedBooksTest::survivesDamagedPlaces()
{
    Damage damage(20261116);
    EpubPackage::Zip zip;
    QString error;
    QVERIFY(EpubPackage::readZip(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/cfi.epub"), &zip, &error));
    int pages = 0;
    for (const QString &name : std::as_const(zip.names)) {
        if (!name.endsWith(QLatin1String("html")))
            continue;
        const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(zip.data.value(name));
        if (!root)
            continue;
        ++pages;
        const EpubCfi::Document document(*root);
        const EpubCfi::PageText text = EpubCfi::PageText::of(*root);
        QStringList cfis{ QStringLiteral("/"), QStringLiteral("/0"), QStringLiteral("/99999999999999"),
                          QStringLiteral("/2/4[x]/1:-1"), QStringLiteral("/2/4:99999999"), QStringLiteral("/2[^[^]^]"),
                          QStringLiteral("/2/4/6[note]/1:22"), QString(4000, u'/'), QStringLiteral("/2/4~1.5@3:4") };
        for (int n = 0; n < 40 && !text.starts.isEmpty(); ++n) {
            const EpubCfi::Point point = text.starts.at(damage.below(int(text.starts.size())));
            cfis.append(document.encode(*point.node, point.offset));
        }
        for (int round = 0, count = rounds(400); round < count; ++round) {
            const QString cfi = QString::fromUtf8(damage.bytes(cfis.at(damage.below(int(cfis.size()))).toUtf8()));
            const EpubCfi::Point start = document.decode(cfi);
            const EpubCfi::Point end = document.decode(cfis.at(damage.below(int(cfis.size()))));
            if (start.node && end.node) {
                document.textBetween(start, end);
                text.indexOf(start);
            }
            EpubCfi::unescape(cfi);
        }
    }
    QVERIFY(pages > 0);
}

// A PDF's metadata: its XMP packet damaged, and a few whole files, read
// through pdfinfo when it is there.
void DamagedBooksTest::survivesDamagedPdfs()
{
    Damage damage(20261117);
    const QStringList pdfs = files(QStringLiteral(COUSOIR_TEST_DATA_DIR "/import"), { QStringLiteral("*.pdf") });
    QVERIFY(pdfs.size() >= 3);
    int packets = 0;
    for (const QString &path : pdfs) {
        const QByteArray data = readAll(path);
        const qsizetype start = data.indexOf("<x:xmpmeta");
        const qsizetype end = data.indexOf("</x:xmpmeta>", start);
        if (start < 0 || end < 0)
            continue;
        ++packets;
        const QByteArray packet = data.mid(start, end + 12 - start);
        QVERIFY(PdfReader::readXmp(packet, QStringLiteral("fr")));
        for (int round = 0, count = rounds(150); round < count; ++round)
            PdfReader::readXmp(damage.markup(packet), QStringLiteral("fr"));
    }
    QVERIFY(packets >= 3);

    const CalibreConfig config = frenchConfig();
    for (const QString &path : pdfs) {
        for (int round = 0, count = rounds(3); round < count; ++round) {
            const QString damaged = m_dir.filePath(QStringLiteral("damaged.pdf"));
            QVERIFY(writeAll(damaged, damage.bytes(readAll(path))));
            ImportedMetadata metadata;
            QString error;
            BookImport::readFileMetadata(damaged, config, &metadata, &error);
        }
    }
}

// FictionBooks, zipped or not, and plain text damaged: read by the import
// and made into EPUBs, which open.
void DamagedBooksTest::survivesDamagedFictionBooksAndText()
{
    Damage damage(20261118);
    const CalibreConfig config = frenchConfig();
    const QStringList books = files(QStringLiteral(COUSOIR_TEST_DATA_DIR "/import"),
                                    { QStringLiteral("*.fb2"), QStringLiteral("*.fbz"), QStringLiteral("*.txt") });
    QVERIFY(books.size() >= 5);
    int made = 0;
    for (const QString &path : books) {
        const QString suffix = QFileInfo(path).suffix();
        const bool zipped = suffix == QLatin1String("fbz");
        EpubPackage::Zip zip;
        QString error;
        if (zipped)
            QVERIFY(EpubPackage::readZip(path, &zip, &error) && !zip.names.isEmpty());
        const QByteArray original = readAll(path);
        for (int round = 0, count = rounds(40); round < count; ++round) {
            const QString damaged = m_dir.filePath(QStringLiteral("damaged.") + suffix);
            if (zipped && round % 4) {
                EpubPackage::Zip copy = zip;
                const QString name = copy.names.first();
                copy.data[name] = damage.markup(copy.data.value(name));
                QVERIFY(EpubPackage::writeZip(damaged, copy, &error));
            } else {
                QVERIFY(writeAll(damaged, suffix == QLatin1String("txt") ? damage.bytes(original) : damage.markup(original)));
            }
            ImportedMetadata metadata;
            error.clear();
            BookImport::readFileMetadata(damaged, config, &metadata, &error);

            const QString epub = m_dir.filePath(QStringLiteral("made.epub"));
            QFile::remove(epub);
            error.clear();
            const bool ok = suffix == QLatin1String("txt") ? TextBookEpub::txtToEpub(damaged, epub, &error)
                                                           : TextBookEpub::fb2ToEpub(damaged, epub, &error);
            if (!ok)
                continue;
            ++made;
            Damage reading(round);
            const std::unique_ptr<EpubBook> book = EpubBook::open(epub, &error);
            QVERIFY2(book, qPrintable(QStringLiteral("%1, round %2: %3").arg(path).arg(round).arg(error)));
            readAsTheReaderDoes(*book, &reading);
        }
    }
    QVERIFY(made > 0);
}

QTEST_GUILESS_MAIN(DamagedBooksTest)

#include "damaged_books_test.moc"
