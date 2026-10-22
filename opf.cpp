// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/metadata/opf2.py: metadata_to_opf(),
//   serialize_user_metadata()
// - src/calibre/ebooks/metadata/__init__.py: fmt_sidx()
// - src/calibre/ebooks/metadata/book/json_codec.py: encode_is_multiple()
// - src/calibre/library/field_metadata.py: add_custom_field()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "opf.h"

#include <QXmlStreamReader>

#include <algorithm>
#include <numeric>

#include "calibre_db.h"
#include "calibre_text.h"
#include "python_json.h"

namespace {

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
    return PythonJson::dumps(PythonJson::Value::from(text));
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

// Calibre 9.16's built-in fields in a book's record, after which the custom
// columns take their rec_index.
constexpr int builtInFields = 23;

// serialize_user_metadata(): each custom column, by key, as its field
// metadata with the book's value, in json.dumps()'s spelling; a composite's
// as its template made it.
QList<QPair<QString, QString>> userMetadata(const LibraryData &data, const Book &book)
{
    // rec_index: the columns by label, a series' numbers next to it.
    QList<qsizetype> order(data.customColumns.size());
    std::iota(order.begin(), order.end(), qsizetype(0));
    std::sort(order.begin(), order.end(), [&data](qsizetype a, qsizetype b) {
        return data.customColumns.at(a).label < data.customColumns.at(b).label;
    });
    QHash<qsizetype, int> recIndex;
    int next = builtInFields;
    for (const qsizetype index : std::as_const(order)) {
        recIndex.insert(index, next);
        next += data.customColumns.at(index).datatype == QLatin1String("series") ? 2 : 1;
    }

    QList<QPair<QString, QString>> result;
    for (const qsizetype index : std::as_const(order)) {
        const CustomColumn &column = data.customColumns.at(index);
        const QString key = u'#' + column.label;
        const QString &type = column.datatype;
        const bool composite = type == QLatin1String("composite");
        const PythonJson::Value display = PythonJson::parse(column.display);

        PythonJson::Value field = PythonJson::Value::dict();
        field.insert(QStringLiteral("table"), PythonJson::Value::from(QStringLiteral("custom_column_%1").arg(column.id)));
        field.insert(QStringLiteral("column"), PythonJson::Value::from(QStringLiteral("value")));
        field.insert(QStringLiteral("datatype"), PythonJson::Value::from(type));
        // encode_is_multiple(): a character here, the lists' separators in
        // is_multiple2.
        field.insert(QStringLiteral("is_multiple"), column.multiple
                         ? PythonJson::Value::from(composite ? QStringLiteral(",") : QStringLiteral("|"))
                         : PythonJson::Value::null());
        field.insert(QStringLiteral("kind"), PythonJson::Value::from(QStringLiteral("field")));
        field.insert(QStringLiteral("name"), PythonJson::Value::from(column.name));
        field.insert(QStringLiteral("search_terms"), PythonJson::Value::from(QStringList{ key }));
        field.insert(QStringLiteral("label"), PythonJson::Value::from(column.label));
        field.insert(QStringLiteral("colnum"), PythonJson::Value::from(column.id));
        field.insert(QStringLiteral("display"), display.kind == PythonJson::Value::Kind::Dict ? display
                                                                                            : PythonJson::Value::dict());
        field.insert(QStringLiteral("is_custom"), PythonJson::Value::from(true));
        field.insert(QStringLiteral("is_category"), PythonJson::Value::from(column.normalized && !composite));
        field.insert(QStringLiteral("link_column"), PythonJson::Value::from(QStringLiteral("value")));
        field.insert(QStringLiteral("category_sort"), PythonJson::Value::from(QStringLiteral("value")));
        field.insert(QStringLiteral("is_csp"), PythonJson::Value::from(false));
        field.insert(QStringLiteral("is_editable"), PythonJson::Value::from(column.editable));
        field.insert(QStringLiteral("rec_index"), PythonJson::Value::from(recIndex.value(index)));

        const CustomValue value = book.custom.value(index);
        PythonJson::Value stored;
        if (composite) {
            // The value computed from the template, as text.
            stored = PythonJson::Value::from(value.value.toString());
        } else if (column.multiple) {
            stored = PythonJson::Value::from(value.value.toStringList());
        } else if (value.value.isNull()) {
            stored = PythonJson::Value::null();
        } else if (type == QLatin1String("int") || type == QLatin1String("rating")) {
            stored = PythonJson::Value::from(value.value.toLongLong());
        } else if (type == QLatin1String("float")) {
            stored = PythonJson::Value::from(value.value.toDouble());
        } else if (type == QLatin1String("bool")) {
            stored = PythonJson::Value::from(value.value.toBool());
        } else if (type == QLatin1String("datetime")) {
            // to_json() of a date.
            stored = PythonJson::Value::dict();
            stored.insert(QStringLiteral("__class__"), PythonJson::Value::from(QStringLiteral("datetime.datetime")));
            stored.insert(QStringLiteral("__value__"),
                          PythonJson::Value::from(CalibreDb::isoFormat(value.value.toDateTime())));
        } else {
            stored = PythonJson::Value::from(value.value.toString());
        }
        field.insert(QStringLiteral("#value#"), stored);
        if (!composite) {
            field.insert(QStringLiteral("#extra#"), type == QLatin1String("series") && !value.value.isNull()
                                                        ? PythonJson::Value::from(value.seriesIndex)
                                                        : PythonJson::Value::null());
        }
        field.insert(QStringLiteral("is_multiple2"), CalibreDb::multipleSeparators(column));
        result.append({ QStringLiteral("calibre:user_metadata:") + key, PythonJson::dumps(field) });
    }
    return result;
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
        const QString value = CalibreText::strip(CalibreText::cleanAsciiChars(text));
        const QString open = u'<' + tag + attributes;
        lines.append(value.isEmpty() ? open + QStringLiteral("/>")
                                     : open + u'>' + escapeText(value) + QStringLiteral("</") + tag + u'>');
    };
    const auto attribute = [](const QString &name, const QString &value) {
        return QStringLiteral(" %1=\"%2\"").arg(name, escapeAttribute(CalibreText::cleanAsciiChars(value)));
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
    // The user categories, the custom columns, then the annotations, as
    // metadata_to_opf() writes them.
    const QList<QPair<QString, QString>> kept = keptMetadata(previous);
    for (const auto &[name, content] : kept) {
        if (name == QLatin1String("calibre:user_categories"))
            meta(name, content);
    }
    for (const auto &[name, content] : userMetadata(data, book))
        meta(name, content);
    for (const auto &[name, content] : kept) {
        if (name == QLatin1String("calibre:annotation"))
            meta(name, content);
    }

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
