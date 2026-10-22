#include "opds.h"

#include <QDateTime>
#include <QHash>
#include <QTimeZone>
#include <QUrl>
#include <QXmlStreamWriter>

#include <algorithm>

#include "book.h"
#include "calibre_db.h"
#include "calibre_text.h"
#include "description.h"

namespace Opds {

namespace {

const QString navigationType = QStringLiteral("application/atom+xml;profile=opds-catalog;kind=navigation");
const QString acquisitionType = QStringLiteral("application/atom+xml;profile=opds-catalog;kind=acquisition");

QString isoDate(const QDateTime &date)
{
    return date.toUTC().toString(Qt::ISODate);
}

// A search for the books of an item, in Calibre's search language.
QString itemQuery(const QString &field, const QString &value)
{
    QString quoted = value;
    quoted.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
    return QStringLiteral("%1:\"=%2\"").arg(field, quoted);
}

// A query string, its values fully encoded: "+" too, which a server reads
// as a space.
QString queryString(const QList<QPair<QString, QString>> &items)
{
    QStringList pairs;
    for (const auto &[key, value] : items) {
        if (!value.isEmpty())
            pairs.append(key + u'=' + QString::fromLatin1(QUrl::toPercentEncoding(value)));
    }
    return pairs.join(u'&');
}

QString booksPath(const QString &query, const QString &sort, const QString &title)
{
    return QStringLiteral("/opds/books?")
        + queryString({ { QStringLiteral("q"), query }, { QStringLiteral("sort"), sort }, { QStringLiteral("title"), title } });
}

void startFeed(QXmlStreamWriter &xml, const QString &id, const QString &title, const QString &self, const QString &selfType,
               const QDateTime &updated)
{
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("feed"));
    xml.writeDefaultNamespace(QStringLiteral("http://www.w3.org/2005/Atom"));
    xml.writeNamespace(QStringLiteral("http://purl.org/dc/terms/"), QStringLiteral("dc"));
    xml.writeNamespace(QStringLiteral("http://opds-spec.org/2010/catalog"), QStringLiteral("opds"));
    xml.writeNamespace(QStringLiteral("http://a9.com/-/spec/opensearch/1.1/"), QStringLiteral("opensearch"));
    xml.writeTextElement(QStringLiteral("id"), id);
    xml.writeTextElement(QStringLiteral("title"), title);
    xml.writeTextElement(QStringLiteral("updated"), isoDate(updated));
    xml.writeStartElement(QStringLiteral("author"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("Cousoir"));
    xml.writeEndElement();
    const auto link = [&](const QString &rel, const QString &href, const QString &type) {
        xml.writeEmptyElement(QStringLiteral("link"));
        xml.writeAttribute(QStringLiteral("rel"), rel);
        xml.writeAttribute(QStringLiteral("href"), href);
        xml.writeAttribute(QStringLiteral("type"), type);
    };
    link(QStringLiteral("self"), self, selfType);
    link(QStringLiteral("start"), QStringLiteral("/opds"), navigationType);
    link(QStringLiteral("search"), QStringLiteral("/opds/search.xml"), QStringLiteral("application/opensearchdescription+xml"));
}

void navigationEntry(QXmlStreamWriter &xml, const QString &id, const QString &title, const QString &content,
                     const QString &href, const QString &type, const QDateTime &updated)
{
    xml.writeStartElement(QStringLiteral("entry"));
    xml.writeTextElement(QStringLiteral("title"), title);
    xml.writeTextElement(QStringLiteral("id"), id);
    xml.writeTextElement(QStringLiteral("updated"), isoDate(updated));
    if (!content.isEmpty()) {
        xml.writeStartElement(QStringLiteral("content"));
        xml.writeAttribute(QStringLiteral("type"), QStringLiteral("text"));
        xml.writeCharacters(content);
        xml.writeEndElement();
    }
    xml.writeEmptyElement(QStringLiteral("link"));
    xml.writeAttribute(QStringLiteral("rel"), QStringLiteral("subsection"));
    xml.writeAttribute(QStringLiteral("href"), href);
    xml.writeAttribute(QStringLiteral("type"), type);
    xml.writeEndElement();
}

QDateTime latest(const LibraryData &data)
{
    QDateTime newest = QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC);
    for (const Book &book : data.books)
        newest = std::max(newest, book.lastModified);
    return newest;
}

QString escape(const QString &text)
{
    return text.toHtmlEscaped();
}

// rating_to_stars(): two ratings make a star, an odd one ends in a half.
QString stars(int rating)
{
    return QString(rating / 2, u'★') + (rating % 2 ? QStringLiteral("½") : QString());
}

} // namespace

QString mimeType(const QString &format)
{
    static const QHash<QString, QString> types{
        { QStringLiteral("EPUB"), QStringLiteral("application/epub+zip") },
        { QStringLiteral("KEPUB"), QStringLiteral("application/kepub+zip") },
        { QStringLiteral("PDF"), QStringLiteral("application/pdf") },
        { QStringLiteral("AZW3"), QStringLiteral("application/vnd.amazon.mobi8-ebook") },
        { QStringLiteral("AZW"), QStringLiteral("application/vnd.amazon.ebook") },
        { QStringLiteral("MOBI"), QStringLiteral("application/x-mobipocket-ebook") },
        { QStringLiteral("CBZ"), QStringLiteral("application/vnd.comicbook+zip") },
        { QStringLiteral("CBR"), QStringLiteral("application/vnd.comicbook-rar") },
        { QStringLiteral("FB2"), QStringLiteral("application/x-fictionbook+xml") },
        { QStringLiteral("TXT"), QStringLiteral("text/plain") },
        { QStringLiteral("RTF"), QStringLiteral("application/rtf") },
        { QStringLiteral("DJVU"), QStringLiteral("image/vnd.djvu") },
        { QStringLiteral("DOCX"), QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document") },
        { QStringLiteral("HTMLZ"), QStringLiteral("application/zip") },
    };
    return types.value(format.toUpper(), QStringLiteral("application/octet-stream"));
}

QByteArray rootFeed(const LibraryData &data, const QString &libraryName)
{
    QByteArray out;
    QXmlStreamWriter xml(&out);
    const QDateTime updated = latest(data);
    const QString id = QStringLiteral("urn:uuid:") + data.uuid;
    startFeed(xml, id, libraryName, QStringLiteral("/opds"), navigationType, updated);
    const auto entry = [&](const QString &key, const QString &title, const QString &content, const QString &href,
                           const QString &type) {
        navigationEntry(xml, id + u':' + key, title, content, href, type, updated);
    };
    entry(QStringLiteral("new"), QObject::tr("Newest"), QObject::tr("The books added last"),
          booksPath({}, QStringLiteral("timestamp"), QObject::tr("Newest")), acquisitionType);
    entry(QStringLiteral("title"), QObject::tr("By title"), QObject::tr("Every book, %n in all", nullptr, int(data.books.size())),
          booksPath({}, QStringLiteral("title"), QObject::tr("By title")), acquisitionType);
    entry(QStringLiteral("authors"), QObject::tr("Authors"), QObject::tr("The books by author"),
          QStringLiteral("/opds/authors"), navigationType);
    entry(QStringLiteral("series"), QObject::tr("Series"), QObject::tr("The books by series"),
          QStringLiteral("/opds/series"), navigationType);
    entry(QStringLiteral("tags"), QObject::tr("Tags"), QObject::tr("The books by tag"), QStringLiteral("/opds/tags"),
          navigationType);
    entry(QStringLiteral("publishers"), QObject::tr("Publishers"), QObject::tr("The books by publisher"),
          QStringLiteral("/opds/publishers"), navigationType);
    entry(QStringLiteral("languages"), QObject::tr("Languages"), QObject::tr("The books by language"),
          QStringLiteral("/opds/languages"), navigationType);
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

QByteArray booksFeed(const LibraryData &data, const QList<qsizetype> &rows, const QString &title, const QString &path,
                     int offset)
{
    QByteArray out;
    QXmlStreamWriter xml(&out);
    const QString separator = path.contains(u'?') ? QStringLiteral("&") : QStringLiteral("?");
    const QString self = offset > 0 ? path + separator + QStringLiteral("offset=%1").arg(offset) : path;
    startFeed(xml, QStringLiteral("urn:uuid:%1:%2").arg(data.uuid, path), title, self, acquisitionType, latest(data));
    xml.writeTextElement(QStringLiteral("opensearch:totalResults"), QString::number(rows.size()));
    xml.writeTextElement(QStringLiteral("opensearch:startIndex"), QString::number(offset + 1));
    xml.writeTextElement(QStringLiteral("opensearch:itemsPerPage"), QString::number(pageSize));
    const auto page = [&](const QString &rel, int at) {
        xml.writeEmptyElement(QStringLiteral("link"));
        xml.writeAttribute(QStringLiteral("rel"), rel);
        xml.writeAttribute(QStringLiteral("href"), at > 0 ? path + separator + QStringLiteral("offset=%1").arg(at) : path);
        xml.writeAttribute(QStringLiteral("type"), acquisitionType);
    };
    if (offset > 0)
        page(QStringLiteral("previous"), std::max(0, offset - pageSize));
    if (offset + pageSize < rows.size())
        page(QStringLiteral("next"), offset + pageSize);
    for (qsizetype i = offset; i < rows.size() && i < offset + pageSize; ++i) {
        const Book &book = data.books.at(rows.at(i));
        xml.writeStartElement(QStringLiteral("entry"));
        xml.writeTextElement(QStringLiteral("title"), book.title);
        xml.writeTextElement(QStringLiteral("id"), QStringLiteral("urn:uuid:") + book.uuid);
        xml.writeTextElement(QStringLiteral("updated"), isoDate(book.lastModified));
        if (!CalibreDb::isUndefinedDate(book.timestamp))
            xml.writeTextElement(QStringLiteral("published"), isoDate(book.timestamp));
        for (const QString &author : book.authors) {
            xml.writeStartElement(QStringLiteral("author"));
            xml.writeTextElement(QStringLiteral("name"), author);
            xml.writeEndElement();
        }
        for (const QString &language : book.languages)
            xml.writeTextElement(QStringLiteral("dc:language"), language);
        if (!book.publisher.isEmpty())
            xml.writeTextElement(QStringLiteral("dc:publisher"), book.publisher);
        if (!CalibreDb::isUndefinedDate(book.pubdate))
            xml.writeTextElement(QStringLiteral("dc:issued"), book.pubdate.toUTC().date().toString(Qt::ISODate));
        for (const auto &[type, value] : book.identifiers) {
            if (type.compare(QLatin1String("isbn"), Qt::CaseInsensitive) == 0)
                xml.writeTextElement(QStringLiteral("dc:identifier"), QStringLiteral("urn:isbn:") + value);
        }
        for (const QString &tag : book.tags) {
            xml.writeEmptyElement(QStringLiteral("category"));
            xml.writeAttribute(QStringLiteral("term"), tag);
            xml.writeAttribute(QStringLiteral("label"), tag);
        }
        // As Calibre's content server describes a book: rating, tags, series,
        // then its description.
        QStringList content;
        if (book.rating > 0)
            content.append(QObject::tr("Rating: %1").arg(escape(stars(book.rating))));
        if (!book.tags.isEmpty())
            content.append(QObject::tr("Tags: %1").arg(escape(book.tags.join(QStringLiteral(", ")))));
        if (!book.series.isEmpty())
            content.append(QObject::tr("Series: %1 [%2]")
                               .arg(escape(book.series), CalibreText::seriesIndexText(book.seriesIndex)));
        QString html = content.join(QStringLiteral("<br />\n"));
        if (!book.comments.trimmed().isEmpty())
            html += (html.isEmpty() ? QString() : QStringLiteral("<br />\n")) + Description::cleaned(book.comments);
        if (!html.isEmpty()) {
            xml.writeStartElement(QStringLiteral("content"));
            xml.writeAttribute(QStringLiteral("type"), QStringLiteral("html"));
            xml.writeCharacters(html);
            xml.writeEndElement();
        }
        if (!book.coverFile.isEmpty()) {
            // The names of OPDS 1.0 too, which older readers look for.
            const auto image = [&](const char *rel, const char *path) {
                xml.writeEmptyElement(QStringLiteral("link"));
                xml.writeAttribute(QStringLiteral("rel"), QString::fromLatin1(rel));
                xml.writeAttribute(QStringLiteral("href"), QStringLiteral("/%1/%2").arg(QLatin1String(path)).arg(book.id));
                xml.writeAttribute(QStringLiteral("type"), QStringLiteral("image/jpeg"));
            };
            image("http://opds-spec.org/image", "cover");
            image("http://opds-spec.org/image/thumbnail", "thumb");
            image("http://opds-spec.org/cover", "cover");
            image("http://opds-spec.org/thumbnail", "thumb");
        }
        for (const BookFormat &format : book.formats) {
            if (format.file.isEmpty())
                continue;
            xml.writeEmptyElement(QStringLiteral("link"));
            xml.writeAttribute(QStringLiteral("rel"), QStringLiteral("http://opds-spec.org/acquisition"));
            xml.writeAttribute(QStringLiteral("href"), QStringLiteral("/get/%1/%2").arg(book.id).arg(format.format));
            xml.writeAttribute(QStringLiteral("type"), mimeType(format.format));
            xml.writeAttribute(QStringLiteral("title"), format.format);
            xml.writeAttribute(QStringLiteral("length"), QString::number(format.size));
        }
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

QByteArray categoryFeed(const LibraryData &data, const QString &kind, const QString &interfaceLanguage)
{
    const QList<FacetItem> *items = kind == QLatin1String("authors") ? &data.authors
        : kind == QLatin1String("series")                            ? &data.series
        : kind == QLatin1String("tags")                              ? &data.tags
        : kind == QLatin1String("publishers")                        ? &data.publishers
        : kind == QLatin1String("languages")                         ? &data.languages
                                                                     : nullptr;
    if (!items)
        return {};
    const QHash<QString, QString> fields{ { QStringLiteral("authors"), QStringLiteral("authors") },
                                          { QStringLiteral("series"), QStringLiteral("series") },
                                          { QStringLiteral("tags"), QStringLiteral("tags") },
                                          { QStringLiteral("publishers"), QStringLiteral("publisher") },
                                          { QStringLiteral("languages"), QStringLiteral("languages") } };
    const QHash<QString, QString> titles{ { QStringLiteral("authors"), QObject::tr("Authors") },
                                          { QStringLiteral("series"), QObject::tr("Series") },
                                          { QStringLiteral("tags"), QObject::tr("Tags") },
                                          { QStringLiteral("publishers"), QObject::tr("Publishers") },
                                          { QStringLiteral("languages"), QObject::tr("Languages") } };
    // Their books' counts.
    QHash<int, int> counts;
    for (const Book &book : data.books) {
        const QList<int> ids = kind == QLatin1String("authors") ? book.authorIds
            : kind == QLatin1String("series")                   ? (book.seriesId > 0 ? QList<int>{ book.seriesId } : QList<int>{})
            : kind == QLatin1String("tags")                     ? book.tagIds
            : kind == QLatin1String("publishers")               ? (book.publisherId > 0 ? QList<int>{ book.publisherId } : QList<int>{})
                                                                : book.languageIds;
        for (const int id : ids)
            ++counts[id];
    }
    QList<FacetItem> sorted;
    for (const FacetItem &item : *items) {
        if (counts.value(item.id) > 0)
            sorted.append(item);
    }
    std::sort(sorted.begin(), sorted.end(), [](const FacetItem &a, const FacetItem &b) {
        const QString x = a.sort.isEmpty() ? a.name : a.sort;
        const QString y = b.sort.isEmpty() ? b.name : b.sort;
        return QString::localeAwareCompare(x, y) < 0;
    });
    QByteArray out;
    QXmlStreamWriter xml(&out);
    const QDateTime updated = latest(data);
    startFeed(xml, QStringLiteral("urn:uuid:%1:%2").arg(data.uuid, kind), titles.value(kind),
              QStringLiteral("/opds/") + kind, navigationType, updated);
    for (const FacetItem &item : std::as_const(sorted)) {
        // A language is searched by its code, the item's sort.
        const bool language = kind == QLatin1String("languages");
        const QString name = language ? CalibreText::languageName(item.sort, interfaceLanguage) : item.name;
        const QString query = itemQuery(fields.value(kind), language ? item.sort : item.name);
        const QString sort = kind == QLatin1String("series") ? QStringLiteral("series") : QStringLiteral("title");
        navigationEntry(xml, QStringLiteral("urn:uuid:%1:%2:%3").arg(data.uuid, kind).arg(item.id), name,
                        QObject::tr("%n book(s)", nullptr, counts.value(item.id)), booksPath(query, sort, name),
                        acquisitionType, updated);
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

QByteArray openSearch(const QString &libraryName)
{
    QByteArray out;
    QXmlStreamWriter xml(&out);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("OpenSearchDescription"));
    xml.writeDefaultNamespace(QStringLiteral("http://a9.com/-/spec/opensearch/1.1/"));
    xml.writeTextElement(QStringLiteral("ShortName"), libraryName);
    xml.writeTextElement(QStringLiteral("Description"), QObject::tr("Search the books, as in Calibre"));
    xml.writeTextElement(QStringLiteral("InputEncoding"), QStringLiteral("UTF-8"));
    xml.writeTextElement(QStringLiteral("OutputEncoding"), QStringLiteral("UTF-8"));
    xml.writeEmptyElement(QStringLiteral("Url"));
    xml.writeAttribute(QStringLiteral("type"), acquisitionType);
    xml.writeAttribute(QStringLiteral("template"), QStringLiteral("/opds/books?q={searchTerms}&sort=title"));
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

QByteArray webPage(const LibraryData &data, const QList<qsizetype> &rows, const QString &query, int offset,
                   const QString &libraryName, const QString &error)
{
    QString html;
    html += QStringLiteral(R"(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>%1</title>
<style>
:root { --bg: #fbfaf8; --fg: #1d1d1f; --muted: #6e6e73; --line: #d8d6d2; --accent: #2f6db5; }
@media (prefers-color-scheme: dark) { :root { --bg: #16161a; --fg: #e6e6e9; --muted: #9a9aa2; --line: #333338; --accent: #7aa7e8; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.4 ui-monospace, "JetBrains Mono", monospace; }
header { position: sticky; top: 0; background: var(--bg); border-bottom: 1px solid var(--line); padding: 12px 16px; display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
header h1 { font-size: 16px; margin: 0; flex: 1 1 auto; }
form { display: flex; gap: 8px; flex: 1 1 320px; }
input { flex: 1; font: inherit; color: var(--fg); background: transparent; border: 1px solid var(--line); padding: 6px 8px; }
button, .format { font: inherit; color: var(--accent); background: transparent; border: 1px solid var(--accent); padding: 4px 8px; text-decoration: none; }
main { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 20px; padding: 16px; }
.book { display: flex; flex-direction: column; gap: 4px; }
.book img, .nocover { width: 100%; aspect-ratio: 2 / 3; object-fit: cover; border: 1px solid var(--line); background: var(--line); }
.title { font-weight: bold; }
.meta { color: var(--muted); font-size: 13px; }
.formats { display: flex; gap: 6px; flex-wrap: wrap; margin-top: 4px; }
nav { display: flex; justify-content: center; gap: 12px; padding: 0 16px 24px; }
.note { padding: 16px; color: var(--muted); }
</style></head><body>
<header><h1>%1</h1><form action="/"><input name="q" value="%2" placeholder="%3"><button>%4</button></form></header>
)").arg(escape(libraryName), escape(query), escape(QObject::tr("Search, as in Calibre")), escape(QObject::tr("Search")));
    if (!error.isEmpty())
        html += QStringLiteral("<p class=\"note\">%1</p>\n").arg(escape(error));
    else if (rows.isEmpty())
        html += QStringLiteral("<p class=\"note\">%1</p>\n").arg(escape(QObject::tr("No book found.")));
    html += QStringLiteral("<main>\n");
    for (qsizetype i = offset; i < rows.size() && i < offset + pageSize; ++i) {
        const Book &book = data.books.at(rows.at(i));
        html += QStringLiteral("<div class=\"book\">");
        if (!book.coverFile.isEmpty())
            html += QStringLiteral("<img loading=\"lazy\" src=\"/thumb/%1\" alt=\"\">").arg(book.id);
        else
            html += QStringLiteral("<div class=\"nocover\"></div>");
        html += QStringLiteral("<div class=\"title\">%1</div>").arg(escape(book.title));
        html += QStringLiteral("<div class=\"meta\">%1</div>").arg(escape(book.authors.join(QStringLiteral(" & "))));
        if (!book.series.isEmpty())
            html += QStringLiteral("<div class=\"meta\">%1</div>")
                        .arg(escape(QObject::tr("%1, book %2").arg(book.series, CalibreText::seriesIndexText(book.seriesIndex))));
        html += QStringLiteral("<div class=\"formats\">");
        for (const BookFormat &format : book.formats) {
            if (!format.file.isEmpty())
                html += QStringLiteral("<a class=\"format\" href=\"/get/%1/%2\">%2</a>").arg(book.id).arg(escape(format.format));
        }
        html += QStringLiteral("</div></div>\n");
    }
    html += QStringLiteral("</main>\n<nav>");
    const auto pageLink = [&](int at, const QString &label) {
        const QString url = queryString({ { QStringLiteral("q"), query },
                                          { QStringLiteral("offset"), at > 0 ? QString::number(at) : QString() } });
        html += QStringLiteral("<a class=\"format\" href=\"/?%1\">%2</a>").arg(escape(url), escape(label));
    };
    if (offset > 0)
        pageLink(std::max(0, offset - pageSize), QObject::tr("Previous"));
    if (offset + pageSize < rows.size())
        pageLink(offset + pageSize, QObject::tr("Next"));
    html += QStringLiteral("</nav>\n</body></html>\n");
    return html.toUtf8();
}

} // namespace Opds
