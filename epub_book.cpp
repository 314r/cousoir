#include "epub_book.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QUrl>
#include <QXmlStreamReader>

#include <zip.h>

namespace {

zip_t *archive(void *zip)
{
    return static_cast<zip_t *>(zip);
}

// The attribute, whatever its namespace.
QString attribute(const QXmlStreamReader &reader, const char *name)
{
    for (const QXmlStreamAttribute &attribute : reader.attributes()) {
        if (attribute.name() == QLatin1String(name))
            return attribute.value().toString();
    }
    return {};
}

} // namespace

std::unique_ptr<EpubBook> EpubBook::open(const QString &path, QString *error)
{
    std::unique_ptr<EpubBook> book(new EpubBook);
    book->m_path = path;
    int code = 0;
    book->m_zip = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &code);
    if (!book->m_zip) {
        *error = QObject::tr("%1 is not an EPUB: no ZIP archive.").arg(QFileInfo(path).fileName());
        return nullptr;
    }
    if (!book->load(error))
        return nullptr;
    return book;
}

EpubBook::~EpubBook()
{
    if (m_zip)
        zip_discard(archive(m_zip));
}

QString EpubBook::resolve(const QString &base, const QString &href)
{
    const QString path = QUrl::fromPercentEncoding(href.section(u'#', 0, 0).toUtf8());
    if (path.isEmpty())
        return base;
    const QString folder = base.contains(u'/') ? base.left(base.lastIndexOf(u'/') + 1) : QString();
    QString resolved = QDir::cleanPath(folder + path);
    while (resolved.startsWith(QLatin1String("../")))
        resolved = resolved.mid(3);
    if (resolved.startsWith(u'/'))
        resolved = resolved.mid(1);
    return resolved;
}

bool EpubBook::contains(const QString &name) const
{
    return m_sizes.contains(name);
}

int EpubBook::spineIndex(const QString &name) const
{
    for (int i = 0; i < m_spine.size(); ++i) {
        if (m_spine.at(i).name == name)
            return i;
    }
    return -1;
}

QByteArray EpubBook::read(const QString &name) const
{
    const QMutexLocker locker(&m_lock);
    zip_file_t *file = zip_fopen(archive(m_zip), name.toUtf8().constData(), 0);
    if (!file)
        return {};
    QByteArray bytes(qsizetype(m_sizes.value(name)), Qt::Uninitialized);
    const zip_int64_t read = zip_fread(file, bytes.data(), zip_uint64_t(bytes.size()));
    zip_fclose(file);
    if (read < 0)
        return {};
    bytes.truncate(qsizetype(read));
    return bytes;
}

QString EpubBook::mediaType(const QString &name) const
{
    const QString type = m_types.value(name);
    if (!type.isEmpty())
        return type;
    static const QMimeDatabase database;
    return database.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
}

bool EpubBook::load(QString *error)
{
    const zip_int64_t count = zip_get_num_entries(archive(m_zip), 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive(m_zip), zip_uint64_t(i), 0, &stat) == 0 && (stat.valid & ZIP_STAT_NAME)
            && (stat.valid & ZIP_STAT_SIZE))
            m_sizes.insert(QString::fromUtf8(stat.name), qint64(stat.size));
    }

    // META-INF/container.xml names the package.
    QString package;
    QXmlStreamReader container(read(QStringLiteral("META-INF/container.xml")));
    while (!container.atEnd() && package.isEmpty()) {
        if (container.readNext() == QXmlStreamReader::StartElement && container.name() == QLatin1String("rootfile")) {
            const QString type = attribute(container, "media-type");
            if (type.isEmpty() || type == QLatin1String("application/oebps-package+xml"))
                package = attribute(container, "full-path");
        }
    }
    if (package.isEmpty() || !contains(package)) {
        *error = QObject::tr("%1 has no package to read.").arg(QFileInfo(m_path).fileName());
        return false;
    }

    QHash<QString, QString> hrefs;
    QString navName;
    QString ncxId;
    struct Itemref
    {
        QString idref;
        bool linear = true;
        QStringList properties;
    };
    QList<Itemref> itemrefs;
    // The book's layout, and whether its pages go two by two.
    bool fixed = false;
    bool spreads = true;
    QXmlStreamReader opf(read(package));
    bool inMetadata = false;
    while (!opf.atEnd()) {
        const QXmlStreamReader::TokenType token = opf.readNext();
        if (token == QXmlStreamReader::EndElement && opf.name() == QLatin1String("metadata"))
            inMetadata = false;
        if (token != QXmlStreamReader::StartElement)
            continue;
        const QStringView name = opf.name();
        if (name == QLatin1String("metadata")) {
            inMetadata = true;
        } else if (inMetadata && name == QLatin1String("title") && m_title.isEmpty()) {
            m_title = opf.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
        } else if (inMetadata && name == QLatin1String("language") && m_language.isEmpty()) {
            m_language = opf.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
        } else if (inMetadata && name == QLatin1String("meta")) {
            const QString property = attribute(opf, "property");
            // EPUB 2's fixed layout, as Kindle and some others write it.
            if (attribute(opf, "name") == QLatin1String("fixed-layout"))
                fixed = attribute(opf, "content") == QLatin1String("true");
            else if (property == QLatin1String("rendition:layout"))
                fixed = opf.readElementText().trimmed() == QLatin1String("pre-paginated");
            else if (property == QLatin1String("rendition:spread"))
                spreads = opf.readElementText().trimmed() != QLatin1String("none");
        } else if (name == QLatin1String("item")) {
            const QString id = attribute(opf, "id");
            const QString file = resolve(package, attribute(opf, "href"));
            hrefs.insert(id, file);
            m_types.insert(file, attribute(opf, "media-type"));
            if (attribute(opf, "properties").split(u' ').contains(QStringLiteral("nav")))
                navName = file;
        } else if (name == QLatin1String("spine")) {
            ncxId = attribute(opf, "toc");
            m_direction = attribute(opf, "page-progression-direction");
        } else if (name == QLatin1String("itemref")) {
            itemrefs.append({ attribute(opf, "idref"), attribute(opf, "linear") != QLatin1String("no"),
                              attribute(opf, "properties").split(u' ', Qt::SkipEmptyParts) });
        }
    }
    // Apple's books say it in a file of their own.
    if (!fixed) {
        QXmlStreamReader options(read(QStringLiteral("META-INF/com.apple.ibooks.display-options.xml")));
        while (!options.atEnd()) {
            if (options.readNext() == QXmlStreamReader::StartElement && options.name() == QLatin1String("option")
                && attribute(options, "name") == QLatin1String("fixed-layout"))
                fixed = options.readElementText().trimmed() == QLatin1String("true");
        }
    }
    // The side of a page the book does not place is across from the one
    // before it; the first of a run, a cover, on the side a book opens to,
    // alone: the right in a book read from left to right.
    const bool rtl = m_direction == QLatin1String("rtl");
    QString before;
    for (const Itemref &itemref : std::as_const(itemrefs)) {
        const QString file = hrefs.value(itemref.idref);
        if (file.isEmpty() || !contains(file))
            continue;
        SpineItem item{ file, itemref.linear, m_sizes.value(file) };
        const QStringList &properties = itemref.properties;
        item.fixed = properties.contains(QStringLiteral("rendition:layout-pre-paginated"))
            || (fixed && !properties.contains(QStringLiteral("rendition:layout-reflowable")));
        if (item.fixed) {
            for (const QString &side : { QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("center") }) {
                if (properties.contains(QStringLiteral("page-spread-") + side)
                    || properties.contains(QStringLiteral("rendition:page-spread-") + side))
                    item.side = side;
            }
            if (!spreads || properties.contains(QStringLiteral("rendition:spread-none")))
                item.side = QStringLiteral("center");
            else if (item.side.isEmpty())
                item.side = before == QLatin1String("left") ? QStringLiteral("right")
                    : before == QLatin1String("right")      ? QStringLiteral("left")
                    : rtl                                   ? QStringLiteral("left")
                                                            : QStringLiteral("right");
        }
        before = item.side;
        m_spine.append(item);
    }
    if (m_spine.isEmpty()) {
        *error = QObject::tr("%1 has nothing to read: its spine is empty.").arg(QFileInfo(m_path).fileName());
        return false;
    }
    if (!navName.isEmpty() && contains(navName))
        readNav(navName);
    if (m_toc.isEmpty() && !ncxId.isEmpty() && contains(hrefs.value(ncxId)))
        readNcx(hrefs.value(ncxId));
    return true;
}

void EpubBook::readNav(const QString &name)
{
    // EPUB 3: the nav element of type toc, its nested lists.
    QXmlStreamReader nav(read(name));
    bool inToc = false;
    int listDepth = 0;
    while (!nav.atEnd()) {
        const QXmlStreamReader::TokenType token = nav.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (nav.name() == QLatin1String("nav")) {
                if (!inToc && attribute(nav, "type") == QLatin1String("toc"))
                    inToc = true;
            } else if (inToc && nav.name() == QLatin1String("ol")) {
                ++listDepth;
            } else if (inToc && (nav.name() == QLatin1String("a") || nav.name() == QLatin1String("span"))) {
                const QString href = attribute(nav, "href");
                const QString title = nav.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                if (!title.isEmpty())
                    m_toc.append({ title, href.isEmpty() ? QString() : resolve(name, href), href.section(u'#', 1),
                                   std::max(0, listDepth - 1) });
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (nav.name() == QLatin1String("nav")) {
                if (inToc)
                    break;
            } else if (inToc && nav.name() == QLatin1String("ol")) {
                --listDepth;
            }
        }
    }
}

void EpubBook::readNcx(const QString &name)
{
    // EPUB 2: the NCX's navMap, its nested navPoints.
    QXmlStreamReader ncx(read(name));
    int depth = -1;
    bool inMap = false;
    QString title;
    while (!ncx.atEnd()) {
        const QXmlStreamReader::TokenType token = ncx.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (ncx.name() == QLatin1String("navMap")) {
                inMap = true;
            } else if (inMap && ncx.name() == QLatin1String("navPoint")) {
                ++depth;
                title.clear();
            } else if (inMap && ncx.name() == QLatin1String("text") && title.isEmpty()) {
                title = ncx.readElementText().simplified();
            } else if (inMap && ncx.name() == QLatin1String("content")) {
                const QString src = attribute(ncx, "src");
                m_toc.append({ title, resolve(name, src), src.section(u'#', 1), std::max(0, depth) });
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (ncx.name() == QLatin1String("navPoint"))
                --depth;
            else if (ncx.name() == QLatin1String("navMap"))
                break;
        }
    }
}
