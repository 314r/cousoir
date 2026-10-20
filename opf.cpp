// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/metadata/opf2.py: metadata_to_opf()
// - src/calibre/ebooks/metadata/__init__.py: fmt_sidx()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "opf.h"

#include <QXmlStreamReader>

#include "calibre_db.h"
#include "calibre_text.h"

namespace {

// clean_ascii_chars(): the control characters but tab, line feed and
// carriage return go, as they do when lxml refuses them.
QString cleanText(const QString &text)
{
    QString result;
    result.reserve(text.size());
    for (const QChar character : text) {
        const char16_t code = character.unicode();
        if ((code < 32 && code != u'\t' && code != u'\n' && code != u'\r') || code == 127)
            continue;
        result.append(character);
    }
    return result;
}

QString pythonStrip(const QString &text)
{
    qsizetype start = 0;
    qsizetype end = text.size();
    const auto space = [](QChar c) { return c.isSpace() || (c.unicode() >= 0x1c && c.unicode() <= 0x1f); };
    while (start < end && space(text.at(start)))
        ++start;
    while (end > start && space(text.at(end - 1)))
        --end;
    return text.mid(start, end - start);
}

// libxml2's escaping of element text and of attribute values.
QString escapeText(const QString &text)
{
    QString result;
    for (const QChar character : text) {
        switch (character.unicode()) {
        case u'&': result += QStringLiteral("&amp;"); break;
        case u'<': result += QStringLiteral("&lt;"); break;
        case u'>': result += QStringLiteral("&gt;"); break;
        case u'\r': result += QStringLiteral("&#13;"); break;
        default: result += character;
        }
    }
    return result;
}

QString escapeAttribute(const QString &text)
{
    QString result;
    for (const QChar character : text) {
        switch (character.unicode()) {
        case u'&': result += QStringLiteral("&amp;"); break;
        case u'<': result += QStringLiteral("&lt;"); break;
        case u'>': result += QStringLiteral("&gt;"); break;
        case u'"': result += QStringLiteral("&quot;"); break;
        case u'\n': result += QStringLiteral("&#10;"); break;
        case u'\r': result += QStringLiteral("&#13;"); break;
        case u'\t': result += QStringLiteral("&#9;"); break;
        default: result += character;
        }
    }
    return result;
}

// json.dumps(ensure_ascii=False) of a string.
QString jsonString(const QString &text)
{
    QString result = QStringLiteral("\"");
    for (const QChar character : text) {
        const char16_t code = character.unicode();
        switch (code) {
        case u'"': result += QStringLiteral("\\\""); break;
        case u'\\': result += QStringLiteral("\\\\"); break;
        case u'\n': result += QStringLiteral("\\n"); break;
        case u'\r': result += QStringLiteral("\\r"); break;
        case u'\t': result += QStringLiteral("\\t"); break;
        case u'\b': result += QStringLiteral("\\b"); break;
        case u'\f': result += QStringLiteral("\\f"); break;
        default:
            if (code < 0x20)
                result += QStringLiteral("\\u%1").arg(int(code), 4, 16, QLatin1Char('0'));
            else
                result += character;
        }
    }
    return result + u'"';
}

// The links of the book's items, as get_all_link_maps_for_book() gathers
// them, in json.dumps()'s spelling; empty without any.
QString linkMaps(const LibraryData &data, const Book &book)
{
    const auto links = [](const QList<FacetItem> &items, const QList<int> &ids) {
        QStringList entries;
        for (const int id : ids) {
            for (const FacetItem &item : items) {
                if (item.id == id && !item.link.isEmpty())
                    entries.append(jsonString(item.name) + QStringLiteral(": ") + jsonString(item.link));
            }
        }
        return entries;
    };
    QStringList fields;
    const auto add = [&](const char *field, const QStringList &entries) {
        if (!entries.isEmpty())
            fields.append(jsonString(QLatin1String(field)) + QStringLiteral(": {") + entries.join(QStringLiteral(", ")) + u'}');
    };
    add("authors", links(data.authors, book.authorIds));
    add("publisher", links(data.publishers, book.publisherId > 0 ? QList<int>{ book.publisherId } : QList<int>{}));
    add("series", links(data.series, book.seriesId > 0 ? QList<int>{ book.seriesId } : QList<int>{}));
    add("tags", links(data.tags, book.tagIds));
    return fields.isEmpty() ? QString() : u'{' + fields.join(QStringLiteral(", ")) + u'}';
}

// fmt_sidx(): "2" for a whole series index, "2.5" otherwise.
QString seriesIndexText(double index)
{
    if (index == std::floor(index))
        return QString::number(qint64(index));
    QString text = QString::number(index, 'f', 2);
    while (text.endsWith(u'0'))
        text.chop(1);
    return text;
}

// The calibre: metadata of `previous` Cousoir has no data for.
QList<QPair<QString, QString>> keptMetadata(const QByteArray &previous)
{
    QList<QPair<QString, QString>> kept;
    QXmlStreamReader reader(previous);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement || reader.name() != QLatin1String("meta"))
            continue;
        const QString name = reader.attributes().value(QLatin1String("name")).toString();
        if (name.startsWith(QLatin1String("calibre:user_metadata:")) || name == QLatin1String("calibre:annotation")
            || name == QLatin1String("calibre:user_categories")) {
            kept.append({ name, reader.attributes().value(QLatin1String("content")).toString() });
        }
    }
    return kept;
}

} // namespace

namespace Opf {

QByteArray metadataToOpf(const LibraryData &data, const Book &book, const Settings &settings, const QByteArray &previous,
                         const ExactDates *exact)
{
    QStringList lines;
    // factory(): an element of the metadata, its text stripped.
    const auto element = [&lines](const QString &tag, const QString &text, const QString &attributes = {}) {
        const QString value = pythonStrip(cleanText(text));
        const QString open = u'<' + tag + attributes;
        lines.append(value.isEmpty() ? open + QStringLiteral("/>")
                                     : open + u'>' + escapeText(value) + QStringLiteral("</") + tag + u'>');
    };
    const auto attribute = [](const QString &name, const QString &value) {
        return QStringLiteral(" %1=\"%2\"").arg(name, escapeAttribute(cleanText(value)));
    };
    const auto meta = [&](const QString &name, const QString &content) {
        lines.append(QStringLiteral("<meta") + attribute(QStringLiteral("name"), name)
                     + attribute(QStringLiteral("content"), content) + QStringLiteral("/>"));
    };

    element(QStringLiteral("dc:identifier"),
            settings.applicationId.isEmpty() ? QString::number(book.id) : settings.applicationId,
            attribute(QStringLiteral("opf:scheme"), QStringLiteral("calibre"))
                + attribute(QStringLiteral("id"), QStringLiteral("calibre_id")));
    element(QStringLiteral("dc:identifier"), book.uuid,
            attribute(QStringLiteral("opf:scheme"), QStringLiteral("uuid")) + attribute(QStringLiteral("id"), QStringLiteral("uuid_id")));
    element(QStringLiteral("dc:title"), book.title);
    for (const QString &author : book.authors) {
        element(QStringLiteral("dc:creator"), author,
                (book.authorSort.isEmpty() ? QString() : attribute(QStringLiteral("opf:file-as"), book.authorSort))
                    + attribute(QStringLiteral("opf:role"), QStringLiteral("aut")));
    }
    element(QStringLiteral("dc:contributor"), settings.producer,
            attribute(QStringLiteral("opf:file-as"), settings.producerId)
                + attribute(QStringLiteral("opf:role"), QStringLiteral("bkp")));
    element(QStringLiteral("dc:date"), exact ? exact->pubdate.isoFormat() : CalibreDb::isoFormat(book.pubdate));
    if (!book.comments.isEmpty())
        element(QStringLiteral("dc:description"), book.comments);
    if (!book.publisher.isEmpty())
        element(QStringLiteral("dc:publisher"), book.publisher);
    for (const auto &identifier : book.identifiers)
        element(QStringLiteral("dc:identifier"), identifier.second,
                attribute(QStringLiteral("opf:scheme"), identifier.first.toUpper()));
    // A book without a language gets the interface's, as two letters.
    QStringList languages = book.languages;
    if (languages.isEmpty())
        languages.append(settings.interfaceLanguage.section(u'_', 0, 0).section(u'-', 0, 0));
    for (const QString &language : std::as_const(languages)) {
        if (!language.isEmpty() && language.toLower() != QLatin1String("und"))
            element(QStringLiteral("dc:language"), language);
    }
    for (const QString &tag : book.tags)
        element(QStringLiteral("dc:subject"), tag);

    const QString links = linkMaps(data, book);
    if (!links.isEmpty())
        meta(QStringLiteral("calibre:link_maps"), links);
    if (book.seriesId > 0) {
        meta(QStringLiteral("calibre:series"), book.series);
        meta(QStringLiteral("calibre:series_index"), seriesIndexText(book.seriesIndex));
    }
    if (book.rating > 0)
        meta(QStringLiteral("calibre:rating"), QString::number(book.rating));
    meta(QStringLiteral("calibre:timestamp"),
         exact ? exact->timestamp.isoFormat() : CalibreDb::isoFormat(book.timestamp));
    if (!book.sort.isEmpty())
        meta(QStringLiteral("calibre:title_sort"), book.sort);
    for (const auto &kept : keptMetadata(previous))
        meta(kept.first, kept.second);

    QString opf = QStringLiteral(
        "<?xml version='1.0' encoding='utf-8'?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"uuid_id\" version=\"2.0\">\n"
        "    <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n");
    for (const QString &line : std::as_const(lines))
        opf += QStringLiteral("        ") + line + u'\n';
    opf += QStringLiteral("    </metadata>\n");
    if (settings.coverReference) {
        opf += QStringLiteral("    <guide>\n        <reference type=\"cover\" title=\"%1\" href=\"cover.jpg\"/>\n"
                              "    </guide>\n")
                   .arg(escapeAttribute(CalibreText::coverText(settings.interfaceLanguage)));
    } else {
        opf += QStringLiteral("    <guide/>\n");
    }
    opf += QStringLiteral("</package>\n");
    return opf.toUtf8();
}

} // namespace Opf
