#include "book_import.h"

#include <QBuffer>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include <zip.h>

#include <algorithm>
#include <functional>

#include "calibre_text.h"
#include "field_writers.h"
#include "pdf_reader.h"

namespace {

bool readWithCalibre(const QString &path, ImportedMetadata *metadata, bool coverOnly, QString *error);

// --- EPUB --------------------------------------------------------------------

// An EPUB's files, read through libzip.
class Zip
{
public:
    explicit Zip(const QString &path)
    {
        int error = 0;
        m_zip = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &error);
    }
    ~Zip()
    {
        if (m_zip)
            zip_discard(m_zip);
    }
    Zip(const Zip &) = delete;
    Zip &operator=(const Zip &) = delete;

    bool isOpen() const { return m_zip != nullptr; }

    bool exists(const QString &name) const
    {
        return m_zip && zip_name_locate(m_zip, name.toUtf8().constData(), 0) >= 0;
    }

    std::optional<QByteArray> read(const QString &name) const
    {
        if (!m_zip)
            return std::nullopt;
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat(m_zip, name.toUtf8().constData(), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE))
            return std::nullopt;
        zip_file_t *file = zip_fopen(m_zip, name.toUtf8().constData(), 0);
        if (!file)
            return std::nullopt;
        QByteArray data(qsizetype(stat.size), Qt::Uninitialized);
        const zip_int64_t read = zip_fread(file, data.data(), stat.size);
        zip_fclose(file);
        if (read < 0 || zip_uint64_t(read) != stat.size)
            return std::nullopt;
        return data;
    }

private:
    zip_t *m_zip = nullptr;
};

// posixpath.normpath(posixpath.join(base, href)): Calibre takes an href as
// it is, without decoding it.
QString joinPath(const QString &base, const QString &href)
{
    const QString joined = base.isEmpty() || href.startsWith(u'/') ? href : base + u'/' + href;
    const QString clean = QDir::cleanPath(joined);
    return clean.isEmpty() ? QStringLiteral(".") : clean;
}

// Encryption of epub.py: the files encryption.xml says are encrypted, but
// for font obfuscation.
QSet<QString> encryptedFiles(const Zip &zip)
{
    QSet<QString> files;
    const std::optional<QByteArray> raw = zip.read(QStringLiteral("META-INF/encryption.xml"));
    QDomDocument document;
    if (!raw || raw->isEmpty() || !document.setContent(*raw, QDomDocument::ParseOption::UseNamespaceProcessing))
        return files;
    const auto find = [](const QDomElement &parent, const QString &name) {
        QList<QDomElement> found;
        std::function<void(const QDomElement &)> walk = [&](const QDomElement &element) {
            for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                if (child.nodeName().contains(name))
                    found.append(child);
                walk(child);
            }
        };
        walk(parent);
        return found;
    };
    for (const QDomElement &method : find(document.documentElement(), QStringLiteral("EncryptionMethod"))) {
        const QString algorithm = method.attribute(QStringLiteral("Algorithm"));
        const QList<QDomElement> references = find(method.parentNode().toElement(), QStringLiteral("CipherReference"));
        if (references.isEmpty())
            continue;
        const QString uri = references.first().attribute(QStringLiteral("URI"));
        if (uri.isEmpty() || algorithm.isEmpty())
            continue;
        if (algorithm != QLatin1String("http://ns.adobe.com/pdf/enc#RC")
            && algorithm != QLatin1String("http://www.idpf.org/2008/embedding"))
            files.insert(uri);
        else
            files.remove(uri);
    }
    return files;
}

bool readEpub(const QString &path, bool kepub, ImportedMetadata *metadata, QString *error)
{
    const QString name = QFileInfo(path).fileName();
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    Zip zip(path);
    if (!zip.isOpen())
        return fail(QObject::tr("%1 is not an EPUB: it is no ZIP archive.").arg(name));
    // Container of epub.py: the first package file that exists.
    const std::optional<QByteArray> raw = zip.read(QStringLiteral("META-INF/container.xml"));
    if (!raw)
        return fail(QObject::tr("%1 has no META-INF/container.xml.").arg(name));
    QDomDocument container;
    if (!container.setContent(*raw, QDomDocument::ParseOption::UseNamespaceProcessing)
        || container.documentElement().attribute(QStringLiteral("version")) != QLatin1String("1.0"))
        return fail(QObject::tr("%1 has an unsupported META-INF/container.xml.").arg(name));
    const auto localName = [](const QDomElement &element) {
        return element.localName().isEmpty() ? element.tagName().section(u':', -1) : element.localName();
    };
    QDomElement rootfiles;
    for (QDomElement child = container.documentElement().firstChildElement(); !child.isNull() && rootfiles.isNull();
         child = child.nextSiblingElement()) {
        if (localName(child) == QLatin1String("rootfiles"))
            rootfiles = child;
    }
    QString opfPath;
    for (QDomElement rootfile = rootfiles.firstChildElement(); !rootfile.isNull(); rootfile = rootfile.nextSiblingElement()) {
        if (localName(rootfile) != QLatin1String("rootfile"))
            continue;
        const QString mediaType = rootfile.attribute(QStringLiteral("media-type"));
        const QString fullPath = rootfile.attribute(QStringLiteral("full-path"));
        if (mediaType.isEmpty() || fullPath.isEmpty())
            return fail(QObject::tr("%1 has an unsupported META-INF/container.xml.").arg(name));
        // Some Kobo EPUBs list package files that are not there.
        if (zip.exists(fullPath) && opfPath.isEmpty() && mediaType == QLatin1String("application/oebps-package+xml"))
            opfPath = fullPath;
    }
    const std::optional<QByteArray> opf = opfPath.isEmpty() ? std::nullopt : zip.read(opfPath);
    if (!opf)
        return fail(QObject::tr("%1 has no package file.").arg(name));
    OpfReader::Package package;
    if (!OpfReader::read(*opf, &package, error))
        return false;
    *metadata = package.metadata;
    metadata->timestamp.reset();

    // get_cover(): the raster cover, unless it is encrypted; else the first
    // spine item when it is a JPEG image, or rendered by Calibre.
    const QString base = opfPath.contains(u'/') ? opfPath.section(u'/', 0, -2) : QString();
    QString cover = package.rasterCover;
    if (kepub && package.version < 3 && !package.epub3RasterCover.isEmpty())
        cover = package.epub3RasterCover;
    const QSet<QString> encrypted = encryptedFiles(zip);
    if (!cover.isEmpty()) {
        cover = joinPath(base, cover);
        if (encrypted.contains(cover))
            return true;
        if (const std::optional<QByteArray> data = zip.read(cover)) {
            metadata->cover = *data;
            return true;
        }
    }
    if (package.firstSpineItem.isEmpty())
        return true;
    const QString first = joinPath(base, package.firstSpineItem);
    if (encrypted.contains(first) || !zip.exists(first))
        return true;
    const std::optional<QByteArray> page = zip.read(first);
    QBuffer buffer;
    buffer.setData(page.value_or(QByteArray()));
    buffer.open(QIODevice::ReadOnly);
    if (QImageReader::imageFormat(&buffer) == "jpeg")
        metadata->cover = page.value_or(QByteArray());
    else
        readWithCalibre(path, metadata, true, nullptr);
    return true;
}

// --- MOBI --------------------------------------------------------------------

quint32 uint32At(const QByteArray &data, qsizetype offset)
{
    return offset + 4 <= data.size() ? qFromBigEndian<quint32>(data.constData() + offset) : 0;
}

quint16 uint16At(const QByteArray &data, qsizetype offset)
{
    return offset + 2 <= data.size() ? qFromBigEndian<quint16>(data.constData() + offset) : 0;
}

// Windows-1252 as Python decodes it with errors="replace": U+FFFD for its
// five unused bytes.
QString decodeCp1252(const QByteArray &bytes)
{
    static const char16_t high[32] = { 0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
                                       0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
                                       0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                       0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178 };
    QString text;
    text.reserve(bytes.size());
    for (const char byte : bytes) {
        const uchar code = uchar(byte);
        text.append(code >= 0x80 && code < 0xa0 ? QChar(high[code - 0x80]) : QChar(code));
    }
    return text;
}

// imghdr's what() among the types Calibre takes a MOBI cover from.
bool isImage(const QByteArray &data)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    const QByteArray format = QImageReader::imageFormat(&buffer).toLower();
    return format == "jpeg" || format == "jpg" || format == "gif" || format == "png" || format == "bmp"
        || format == "webp";
}

// get_metadata() of ebooks/metadata/mobi.py, with MetadataHeader and
// EXTHHeader of ebooks/mobi/reader/headers.py. Old MOBI files without an
// EXTH header keep the title of their header: Calibre would look for
// metadata in their text too.
bool readMobi(const QString &path, ImportedMetadata *metadata, QString *error, const CalibreConfig &config)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray data = file.readAll();
    const QByteArray ident = data.mid(60, 8).toUpper();
    const int sections = data.size() >= 78 ? uint16At(data, 76) : 0;
    if ((ident != "BOOKMOBI" && ident != "TEXTREAD") || sections < 2) {
        if (error)
            *error = QObject::tr("%1 is not a MOBI file.").arg(QFileInfo(path).fileName());
        return false;
    }
    const auto sectionOffset = [&](qint64 number) { return qsizetype(uint32At(data, 78 + number * 8)); };
    const auto section = [&](qint64 number) -> QByteArray {
        if (number < 0 || number >= sections)
            return {};
        const qsizetype start = sectionOffset(number);
        const qsizetype end = number == sections - 1 ? data.size() : sectionOffset(number + 1);
        return start < end && end <= data.size() ? data.mid(start, end - start) : QByteArray();
    };
    const QString unknown = CalibreText::unknownText(config.interfaceLanguage);
    const QByteArray header = section(0);

    // BookHeader: the title, language and images of the MOBI header.
    QString title = unknown;
    quint32 languageCode = 0;
    qint64 firstImage = -1;
    bool utf8 = false;
    std::optional<ImportedMetadata> exth;
    std::optional<quint32> coverOffset;
    if (header.size() > 16) {
        const quint32 length = uint32At(header, 20);
        utf8 = uint32At(header, 28) == 65001;
        const auto decode = [utf8](const QByteArray &bytes) {
            return utf8 ? QString::fromUtf8(bytes) : decodeCp1252(bytes);
        };
        const quint32 titleOffset = uint32At(header, 0x54);
        const quint64 titleEnd = quint64(titleOffset) + uint32At(header, 0x58);
        if (titleEnd < quint64(header.size()))
            title = decode(header.mid(titleOffset, qsizetype(titleEnd - titleOffset)));
        languageCode = uint32At(header, 0x5c);
        firstImage = uint32At(header, 0x6c);
        const QByteArray raw = uint32At(header, 0x80) & 0x40 ? header.mid(16 + length) : QByteArray();
        if (raw.size() >= 12) {
            // EXTHHeader, from a Metadata("Unknown", ["Unknown"]).
            ImportedMetadata mi;
            const auto text = [&](const QByteArray &bytes) { return CalibreText::cleanAsciiChars(decode(bytes)); };
            const auto field = [&](const QByteArray &bytes) {
                return CalibreText::cleanXmlChars(CalibreText::strip(text(bytes)));
            };
            QString exthTitle = title;
            const quint32 count = uint32At(raw, 8);
            qsizetype position = 12;
            for (quint32 item = 0; item < count && position + 8 <= raw.size(); ++item) {
                const quint32 id = uint32At(raw, position);
                const quint32 size = uint32At(raw, position + 4);
                const QByteArray content = raw.mid(position + 8, qsizetype(size) - 8);
                position += size;
                if (size < 8)
                    break;
                switch (id) {
                case 100: {
                    // Amazon's "Last, First", made "First Last".
                    const QString author = field(content);
                    static const QRegularExpression inverted(QStringLiteral("^([^,]+?)") + CalibreText::pythonSpace
                                                             + QStringLiteral("*,") + CalibreText::pythonSpace
                                                             + QStringLiteral("+([^,]+)$"));
                    const QRegularExpressionMatch match = inverted.match(CalibreText::strip(author));
                    if (match.hasMatch()) {
                        mi.authors.append(config.tweaks.authorSortCopyMethod != QLatin1String("copy")
                                              ? match.captured(2) + u' ' + match.captured(1)
                                              : match.captured(0));
                        mi.authorSort = mi.authorSort.isEmpty() || mi.authorSort == unknown
                            ? match.captured(0) : mi.authorSort + QStringLiteral(" & ") + match.captured(0);
                    } else {
                        mi.authors.append(author);
                    }
                    break;
                }
                case 101:
                    mi.publisher = field(content);
                    if (mi.publisher == QLatin1String("Unknown") || mi.publisher == unknown)
                        mi.publisher.clear();
                    break;
                case 103:
                    mi.comments = field(content);
                    break;
                case 104: {
                    const QString isbn = CalibreText::checkIsbn(CalibreText::strip(text(content)).remove(u'-'));
                    if (!isbn.isEmpty())
                        mi.setIdentifier(QStringLiteral("isbn"), isbn);
                    break;
                }
                case 105:
                    for (const QString &tag : CalibreText::cleanXmlChars(text(content)).split(u';')) {
                        if (!mi.tags.contains(CalibreText::strip(tag)))
                            mi.tags.append(CalibreText::strip(tag));
                    }
                    break;
                case 106:
                    if (const std::optional<CalibreDate> date = OpfReader::parseDate(text(content), false))
                        mi.pubdate = date;
                    break;
                case 108:
                    mi.bookProducer = field(content);
                    break;
                case 112: {
                    const QString value = CalibreText::strip(decode(content));
                    if (value.startsWith(QLatin1String("urn:isbn:"), Qt::CaseInsensitive)) {
                        const QString isbn = CalibreText::checkIsbn(value.mid(9));
                        const bool hasIsbn = std::any_of(mi.identifiers.cbegin(), mi.identifiers.cend(),
                                                         [](const auto &i) { return i.first == QLatin1String("isbn"); });
                        if (!isbn.isEmpty() && !hasIsbn)
                            mi.setIdentifier(QStringLiteral("isbn"), isbn);
                    } else if (value.startsWith(QLatin1String("calibre:")) && value.size() > 8) {
                        mi.uuid = value.mid(8);
                    }
                    break;
                }
                case 113:
                    if (std::all_of(content.cbegin(), content.cend(), [](char c) { return uchar(c) < 0x80; }))
                        mi.setIdentifier(QStringLiteral("mobi-asin"), QString::fromLatin1(content));
                    break;
                case 201:
                    if (content.size() >= 4 && uint32At(content, 0) < 0xffffffff)
                        coverOffset = uint32At(content, 0);
                    break;
                case 503:
                    // Amazon's own title, the header's being mangled by
                    // its e-mail service.
                    exthTitle = text(content);
                    break;
                case 524: {
                    const QString language = CalibreText::canonicalizeLanguage(decode(content));
                    if (!language.isEmpty())
                        mi.languages = { language };
                    break;
                }
                default:
                    break;
                }
            }
            if (!exthTitle.isEmpty())
                mi.title = CalibreText::replaceEntities(CalibreText::cleanXmlChars(CalibreText::cleanAsciiChars(exthTitle)));
            if (mi.title == unknown)
                mi.title.clear();
            if (mi.languages.isEmpty()) {
                const QString language = CalibreText::mobiLanguage(int(languageCode & 0xff));
                if (!language.isEmpty())
                    mi.languages = { language };
            }
            exth = mi;
        }
    }

    ImportedMetadata mi;
    if (exth) {
        mi = *exth;
    } else {
        mi.title = QFileInfo(path).fileName();
        if (!title.isEmpty() && title != unknown)
            mi.title = title;
    }
    const QByteArray cover = section(firstImage + qint64(coverOffset.value_or(0)));
    if (!cover.isEmpty() && isImage(cover))
        mi.cover = cover;
    *metadata = mi;
    return true;
}

// --- Calibre -----------------------------------------------------------------

// get_file_type_metadata() of Calibre's plugin for the format, as JSON, and
// its cover: what _get_metadata() of ebooks/metadata/meta.py merges.
const char calibreReader[] = R"(
import json, sys
from calibre.customize.ui import get_file_type_metadata
from calibre.utils.date import isoformat, is_date_undefined

path, ftype, out, cover = sys.argv[-4:]
ftype = ftype.lower()
if ftype in ('html', 'xhtml', 'xhtm', 'xml'):
    ftype = 'html'
if ftype in ('mobi', 'prc', 'azw'):
    ftype = 'mobi'
if ftype in ('odt', 'ods', 'odp', 'odg', 'odf'):
    ftype = 'odt'
with open(path, 'rb') as stream:
    mi = get_file_type_metadata(stream, ftype)

def date(value):
    return None if value is None or is_date_undefined(value) else isoformat(value, as_utc=True)

data = (mi.cover_data or (None, None))[1]
if data:
    with open(cover, 'wb') as f:
        f.write(data)
with open(out, 'w', encoding='utf-8') as f:
    json.dump({
        'title': '' if mi.is_null('title') else mi.title,
        'title_sort': mi.title_sort or '',
        'authors': [] if mi.is_null('authors') else list(mi.authors),
        'author_sort': '' if mi.is_null('author_sort') else mi.author_sort,
        'series': mi.series or '',
        'series_index': mi.series_index,
        'tags': list(mi.tags or []),
        'publisher': mi.publisher or '',
        'pubdate': date(mi.pubdate),
        'timestamp': date(mi.timestamp),
        'uuid': mi.uuid or '',
        'comments': mi.comments or '',
        'languages': [x for x in (mi.languages or []) if x],
        'identifiers': list(mi.get_identifiers().items()),
        'rating': mi.rating,
        'book_producer': mi.book_producer or '',
    }, f)
)";

// Calibre's reader for any format, through calibre-debug.
bool readWithCalibre(const QString &path, ImportedMetadata *metadata, bool coverOnly, QString *error)
{
    const QString program = BookImport::calibreDebug();
    if (program.isEmpty()) {
        if (error)
            *error = QObject::tr("Reliure cannot read %1 files without Calibre.").arg(BookImport::formatOf(path));
        return false;
    }
    QTemporaryDir folder;
    const QString script = folder.filePath(QStringLiteral("reader.py"));
    const QString json = folder.filePath(QStringLiteral("metadata.json"));
    const QString cover = folder.filePath(QStringLiteral("cover"));
    QFile scriptFile(script);
    if (!folder.isValid() || !scriptFile.open(QIODevice::WriteOnly) || scriptFile.write(calibreReader) < 0) {
        if (error)
            *error = QObject::tr("Cannot create a temporary folder.");
        return false;
    }
    scriptFile.close();
    QProcess process;
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, { QStringLiteral("-e"), script, QStringLiteral("--"), QFileInfo(path).absoluteFilePath(),
                             QFileInfo(path).suffix(), json, cover });
    QFile result(json);
    if (!process.waitForFinished(120000) || process.exitCode() != 0 || !result.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Calibre could not read %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    QFile image(cover);
    if (image.open(QIODevice::ReadOnly))
        metadata->cover = image.readAll();
    if (coverOnly)
        return true;
    const QJsonObject read = QJsonDocument::fromJson(result.readAll()).object();
    ImportedMetadata mi;
    const auto list = [&read](const char *key) {
        QStringList values;
        for (const QJsonValue &value : read.value(QLatin1String(key)).toArray())
            values.append(value.toString());
        return values;
    };
    const auto date = [&read](const char *key) -> std::optional<CalibreDate> {
        return OpfReader::parseDate(read.value(QLatin1String(key)).toString(), true);
    };
    mi.title = read.value(QLatin1String("title")).toString();
    mi.titleSort = read.value(QLatin1String("title_sort")).toString();
    mi.authors = list("authors");
    mi.authorSort = read.value(QLatin1String("author_sort")).toString();
    mi.series = read.value(QLatin1String("series")).toString();
    if (read.value(QLatin1String("series_index")).isDouble())
        mi.seriesIndex = read.value(QLatin1String("series_index")).toDouble();
    mi.tags = list("tags");
    mi.publisher = read.value(QLatin1String("publisher")).toString();
    mi.pubdate = date("pubdate");
    mi.timestamp = date("timestamp");
    mi.uuid = read.value(QLatin1String("uuid")).toString();
    mi.comments = read.value(QLatin1String("comments")).toString();
    mi.languages = list("languages");
    for (const QJsonValue &pair : read.value(QLatin1String("identifiers")).toArray())
        mi.identifiers.append({ pair.toArray().at(0).toString(), pair.toArray().at(1).toString() });
    if (read.value(QLatin1String("rating")).isDouble())
        mi.rating = read.value(QLatin1String("rating")).toDouble();
    mi.bookProducer = read.value(QLatin1String("book_producer")).toString();
    mi.cover = metadata->cover;
    *metadata = mi;
    return true;
}

} // namespace

namespace BookImport {

QString formatOf(const QString &path)
{
    return QFileInfo(path).suffix().toUpper();
}

QString calibreDebug()
{
    const QString found = QStandardPaths::findExecutable(QStringLiteral("calibre-debug"));
    if (!found.isEmpty())
        return found;
    for (const QString &candidate : { QStringLiteral("/opt/calibre/calibre-debug"),
                                      QStringLiteral("/Applications/calibre.app/Contents/MacOS/calibre-debug"),
                                      QStringLiteral("C:/Program Files/Calibre2/calibre-debug.exe") }) {
        if (QFileInfo(candidate).isExecutable())
            return candidate;
    }
    return {};
}

bool readFileMetadata(const QString &path, const CalibreConfig &config, ImportedMetadata *metadata, QString *error)
{
    const QString format = formatOf(path).toLower();
    bool ok;
    if (format == QLatin1String("epub") || format == QLatin1String("kepub")) {
        ok = readEpub(path, format == QLatin1String("kepub"), metadata, error);
    } else if (format == QLatin1String("mobi") || format == QLatin1String("azw") || format == QLatin1String("azw3")
               || format == QLatin1String("prc")) {
        ok = readMobi(path, metadata, error, config);
    } else if (format == QLatin1String("pdf")) {
        ok = PdfReader::read(path, config.interfaceLanguage, metadata, error);
    } else {
        ok = readWithCalibre(path, metadata, false, error);
    }
    return ok;
}

ImportedMetadata metadataFromFilename(const QString &path, const CalibreConfig &config)
{
    QString name = QFileInfo(path).fileName();
    name = name.contains(u'.') ? name.left(name.lastIndexOf(u'.')) : QString();
    name.replace(u'_', u' ');
    ImportedMetadata mi;
    QRegularExpression pattern(config.filenamePattern, QRegularExpression::UseUnicodePropertiesOption);
    if (!pattern.isValid())
        pattern = QRegularExpression(QStringLiteral("(?P<title>.+) - (?P<author>[^_]+)"));
    QRegularExpressionMatch match = pattern.match(name);
    if (!match.hasMatch())
        match = QRegularExpression(QStringLiteral("^(?P<title>.+) - (?P<author>[^-]+)$")).match(name);
    if (match.hasMatch()) {
        mi.title = match.captured(QStringLiteral("title"));
        QStringList authors = CalibreText::stringToAuthors(match.captured(QStringLiteral("author")));
        if (config.swapAuthorNames) {
            for (QString &author : authors) {
                QStringList parts = author.contains(u',') ? QStringList{ author.section(u',', 0, 0), author.section(u',', 1) }
                                                         : QStringList{ author.section(u' ', 0, 0, QString::SectionSkipEmpty),
                                                                        author.section(u' ', 1, -1, QString::SectionSkipEmpty) };
                parts.removeAll(QString());
                if (parts.size() > 1)
                    author = (parts.last() + u' ' + parts.first()).trimmed();
            }
        }
        mi.authors = authors;
        mi.series = match.captured(QStringLiteral("series"));
        bool ok = false;
        const double index = match.captured(QStringLiteral("series_index")).toDouble(&ok);
        if (ok)
            mi.seriesIndex = index;
        const QString isbn = match.captured(QStringLiteral("isbn"));
        if (!isbn.isEmpty())
            mi.setIdentifier(QStringLiteral("isbn"), isbn);
        mi.publisher = match.captured(QStringLiteral("publisher"));
        const QString published = match.captured(QStringLiteral("published"));
        if (!published.isEmpty())
            if (const std::optional<QDateTime> date = FieldWriters::parseOnlyDate(published, QDateTime::currentDateTimeUtc()))
                mi.pubdate = CalibreDate::fromDateTime(*date);
        mi.comments = match.captured(QStringLiteral("comments"));
    }
    if (mi.title.isEmpty())
        mi.title = name;
    return mi;
}

ImportedMetadata metadataForAdding(const QString &path, const CalibreConfig &config, QString *warning)
{
    // _get_metadata(): the name's metadata, "Unknown" where it has none,
    // updated with the file's by smart_update().
    const QString unknown = CalibreText::unknownText(config.interfaceLanguage);
    ImportedMetadata base = metadataFromFilename(path, config);
    if (base.authors.isEmpty())
        base.authors = { unknown };
    if (base.title.isEmpty())
        base.title = unknown;
    ImportedMetadata other;
    if (config.readFileMetadata) {
        QString error;
        if (!readFileMetadata(path, config, &other, &error) && warning)
            *warning = error;
    }

    if (!other.title.isEmpty() && other.title != unknown) {
        base.title = other.title;
        base.titleSort = other.titleSort;
    }
    // The name's authors come without a sort: Calibre's "Unknown".
    const bool baseUnknown = base.authors.isEmpty() || (base.authors.size() == 1 && base.authors.first() == unknown
                                                        && (base.authorSort.isEmpty() || base.authorSort == unknown));
    if (!other.authors.isEmpty() && (other.authors.first() != unknown || baseUnknown)) {
        base.authors = other.authors;
        base.authorSort = other.authorSort;
    }
    // copy_not_none() for the fields copied as they are.
    if (!other.series.isEmpty())
        base.series = other.series;
    if (other.seriesIndex)
        base.seriesIndex = other.seriesIndex;
    if (other.rating)
        base.rating = other.rating;
    if (!other.publisher.isEmpty())
        base.publisher = other.publisher;
    if (other.pubdate)
        base.pubdate = other.pubdate;
    if (other.timestamp)
        base.timestamp = other.timestamp;
    if (!other.uuid.isEmpty())
        base.uuid = other.uuid;
    if (!other.bookProducer.isEmpty())
        base.bookProducer = other.bookProducer;
    // Tags merged without regard to case: the file's spelling replaces the
    // first tag of the same name, its other tags are added.
    QStringList lowerOther;
    QStringList lowerBase;
    for (const QString &tag : std::as_const(other.tags))
        lowerOther.append(tag.toLower());
    for (const QString &tag : std::as_const(base.tags))
        lowerBase.append(tag.toLower());
    for (qsizetype index = 0; index < lowerBase.size(); ++index) {
        const qsizetype found = lowerOther.indexOf(lowerBase.at(index));
        if (found >= 0 && lowerBase.indexOf(lowerBase.at(index)) == index)
            base.tags[index] = other.tags.at(found);
    }
    for (qsizetype index = 0; index < other.tags.size(); ++index) {
        if (!lowerBase.contains(lowerOther.at(index)))
            base.tags.append(other.tags.at(index));
    }
    if (other.cover.size() > base.cover.size())
        base.cover = other.cover;
    if (CalibreText::strip(other.comments).size() > CalibreText::strip(base.comments).size())
        base.comments = other.comments;
    for (const auto &identifier : std::as_const(other.identifiers)) {
        if (!identifier.second.isNull())
            base.setIdentifier(identifier.first, identifier.second);
    }
    if (!other.languages.isEmpty() && other.languages != QStringList{ QStringLiteral("und") })
        base.languages = other.languages;
    if (base.series.isEmpty())
        base.seriesIndex.reset();

    // An "Unknown" sort is none: Calibre computes one.
    if (base.authorSort == unknown)
        base.authorSort.clear();
    // calibredb add's own defaults.
    if (base.title.isEmpty())
        base.title = QFileInfo(path).completeBaseName();
    if (base.authors.isEmpty())
        base.authors = { unknown };
    return base;
}

} // namespace BookImport
