#include "native_send.h"

// Ported from Calibre 9.16:
// - src/calibre/devices/utils.py: create_upload_path();
// - src/calibre/library/save_to_disk.py: get_components(),
//   get_component_metadata();
// - src/calibre/utils/filenames.py: shorten_components_to(),
//   shorten_component(), limit_component();
// - src/calibre/ebooks/metadata/book/json_codec.py: a book's entry in
//   metadata.calibre;
// - src/calibre/utils/img.py: thumbnail(), approximately.
// Copyright (C) Kovid Goyal, GPL v3.

#include <QBuffer>
#include <QDateTime>
#include <QImage>

#include <cmath>

#include "book.h"
#include "calibre_db.h"
#include "calibre_template.h"
#include "calibre_text.h"
#include "device_books.h"
#include "opf.h"

namespace NativeSend {

namespace {

// limit_component(): a part of a path at most 254 bytes in UTF-8.
QString limitComponent(const QString &part)
{
    QString text = part;
    while (text.toUtf8().size() > 254)
        text.chop(1);
    return text;
}

// shorten_component(): a part shortened by `by` characters from its middle.
QString shortenComponent(const QString &part, qsizetype by)
{
    const qsizetype size = part.size();
    if (size < by)
        return part;
    const qsizetype keep = (size - by) / 2;
    if (keep <= 0)
        return part;
    return part.left(keep) + part.right(keep);
}

// shorten_components_to(): the parts of a path shortened in proportion
// until it is `length` long.
QStringList shortenComponentsTo(qsizetype length, QStringList components, bool lastHasExtension)
{
    for (QString &component : components)
        component = limitComponent(component);
    const QString path = components.join(u'/');
    const qsizetype extra = path.size() - length;
    if (extra < 1)
        return components;
    QStringList result;
    for (qsizetype i = 0; i < components.size(); ++i) {
        const QString &part = components.at(i);
        const qsizetype delta = qsizetype(std::ceil(double(part.size()) / double(path.size()) * double(extra)));
        const bool last = i == components.size() - 1;
        QString shortened;
        if (delta > part.size()) {
            shortened = last ? part.left(1) : QString();
        } else if (lastHasExtension && last) {
            const qsizetype dot = part.lastIndexOf(u'.');
            const QString base = dot > 0 ? part.left(dot) : part;
            const QString extension = dot > 0 ? part.mid(dot) : QString();
            shortened = shortenComponent(base, delta) + (extension == QLatin1String(".") ? QString() : extension);
            if (shortened.startsWith(u'.'))
                shortened.prepend(u'x');
        } else {
            shortened = shortenComponent(part, delta);
        }
        result.append(shortened);
    }
    return result;
}

} // namespace

QString sendFormat(const Book &book, const QStringList &formatMap, QString *file)
{
    QStringList formats;
    for (const BookFormat &format : book.formats) {
        if (!format.file.isEmpty())
            formats.append(format.format);
    }
    const QString format = DeviceBooks::sendFormat(formats, formatMap);
    if (format.isEmpty())
        return {};
    for (const BookFormat &bookFormat : book.formats) {
        if (bookFormat.format.compare(format, Qt::CaseInsensitive) == 0)
            *file = bookFormat.file;
    }
    return format;
}

QString uploadPath(const LibraryData &data, const Book &book, const QString &extension, const UploadOptions &options,
                   const QString &interfaceLanguage)
{
    // get_component_metadata(): the title by its sort, the series sorted as
    // a title, and no "/" in a value, which would make a folder.
    Book values = book;
    const auto clean = [](QString text) { return text.replace(u'/', u'_').replace(u'\\', u'_'); };
    values.title = clean(book.sort.isEmpty() ? book.title : book.sort);
    values.authors.clear();
    for (const QString &author : book.authors)
        values.authors.append(clean(author));
    values.authorSort = clean(book.authorSort);
    values.series = clean(CalibreText::titleSort(book.series, book.languages.value(0), CalibreText::Tweaks::defaults(),
                                                 interfaceLanguage));
    values.publisher = clean(book.publisher);
    QStringList tags;
    for (const QString &tag : book.tags)
        tags.append(clean(tag));
    values.tags = tags;
    const CalibreTemplate::Library library(data, interfaceLanguage);
    QString error;
    const QString formatted = CalibreTemplate::evaluate(options.saveTemplate, library, values, &error);
    const QString appId = QString::number(book.id);
    QStringList components;
    for (const QString &part : formatted.split(u'/')) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty())
            components.append(CalibreText::asciiFilename(trimmed));
    }
    components.removeAll(QString());
    if (components.isEmpty())
        components.append(appId);
    components = shortenComponentsTo(options.maxLength - appId.size() - 1, components, false);
    components.last() = CalibreText::asciiFilename(components.last() + options.nameSuffix + extension);
    if (components.last().startsWith(u'.') || components.last().startsWith(u'_'))
        components.last() = u'x' + components.last().mid(1);
    if (!options.useSubdirs)
        components = { components.last() };
    for (QString &component : components) {
        // remove_trailing_periods().
        while (component.endsWith(u'.'))
            component = component.chopped(1).trimmed();
        if (component.isEmpty())
            component = QStringLiteral("x");
    }
    components = shortenComponentsTo(options.maxLength - options.prefix.size(), components, true);
    if (options.sanitizeComponents)
        options.sanitizeComponents(&components);
    return components.join(u'/');
}

QImage thumbnailImage(const QImage &cover, int width, int height)
{
    // thumbnail(): within width by height, kept in proportion; grey when
    // the cover is.
    QImage image = cover.width() > width || cover.height() > height
        ? cover.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : cover;
    if (image.allGray())
        image = image.convertToFormat(QImage::Format_Grayscale8);
    return image;
}

QByteArray thumbnailJpeg(const QImage &cover, int width, int height, int quality)
{
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    thumbnailImage(cover, width, height).save(&buffer, "JPEG", quality);
    return jpeg;
}

PythonJson::Value isoDate(const QDateTime &date)
{
    return PythonJson::Value::from(CalibreDb::isUndefinedDate(date) ? QStringLiteral("None") : CalibreDb::isoFormat(date));
}

PythonJson::Value thumbnail(const Book &book, int width, int height)
{
    if (book.coverFile.isEmpty())
        return PythonJson::Value::null();
    const QImage cover(book.coverFile);
    if (cover.isNull())
        return PythonJson::Value::null();
    QImage small = width > 0 ? thumbnailImage(cover, width, height) : cover.scaledToHeight(height, Qt::SmoothTransformation);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    small.save(&buffer, "JPEG", 75);
    PythonJson::Value value;
    value.kind = PythonJson::Value::Kind::List;
    value.items = { PythonJson::Value::from(small.width()), PythonJson::Value::from(small.height()),
                    PythonJson::Value::from(QString::fromLatin1(jpeg.toBase64())) };
    return value;
}

PythonJson::Value bookEntry(const LibraryData &data, const Book &book, const QString &lpath, qint64 size,
                            const PythonJson::Value &thumbnailValue)
{
    using PythonJson::Value;
    const auto textOrNull = [](const QString &text) { return text.isEmpty() ? Value::null() : Value::from(text); };
    Value user = Value::dict();
    for (const auto &[key, json] : Opf::userMetadata(data, book))
        user.insert(key, PythonJson::parse(json.toUtf8()));
    Value sorts = Value::dict();
    for (qsizetype i = 0; i < book.authors.size(); ++i) {
        const int id = book.authorIds.value(i);
        QString sort;
        for (const FacetItem &author : data.authors) {
            if (author.id == id)
                sort = author.sort;
        }
        sorts.insert(book.authors.at(i), Value::from(sort));
    }
    Value identifiers = Value::dict();
    for (const auto &[type, value] : book.identifiers)
        identifiers.insert(type, Value::from(value));
    Value entry = Value::dict();
    entry.insert(QStringLiteral("comments"), textOrNull(book.comments));
    entry.insert(QStringLiteral("size"), Value::from(size));
    entry.insert(QStringLiteral("user_metadata"), user);
    entry.insert(QStringLiteral("series"), textOrNull(book.series));
    entry.insert(QStringLiteral("rights"), Value::null());
    entry.insert(QStringLiteral("link_maps"), Value::dict());
    entry.insert(QStringLiteral("mime"), Value::null());
    entry.insert(QStringLiteral("authors"), Value::from(book.authors));
    entry.insert(QStringLiteral("book_producer"), Value::null());
    entry.insert(QStringLiteral("application_id"), Value::from(book.id));
    entry.insert(QStringLiteral("series_index"), book.series.isEmpty() ? Value::null() : Value::from(book.seriesIndex));
    entry.insert(QStringLiteral("last_modified"), isoDate(book.lastModified));
    entry.insert(QStringLiteral("author_sort_map"), sorts);
    entry.insert(QStringLiteral("user_categories"), Value::dict());
    entry.insert(QStringLiteral("db_id"), Value::null());
    entry.insert(QStringLiteral("uuid"), Value::from(book.uuid));
    entry.insert(QStringLiteral("cover"), Value::null());
    entry.insert(QStringLiteral("tags"), Value::from(book.tags));
    entry.insert(QStringLiteral("lpath"), Value::from(lpath));
    entry.insert(QStringLiteral("identifiers"), identifiers);
    entry.insert(QStringLiteral("timestamp"), isoDate(book.timestamp));
    entry.insert(QStringLiteral("publication_type"), Value::null());
    entry.insert(QStringLiteral("pages"), Value::from(std::max(0, book.pages)));
    entry.insert(QStringLiteral("pubdate"), isoDate(book.pubdate));
    entry.insert(QStringLiteral("publisher"), textOrNull(book.publisher));
    entry.insert(QStringLiteral("thumbnail"), thumbnailValue);
    entry.insert(QStringLiteral("author_sort"), Value::from(book.authorSort));
    entry.insert(QStringLiteral("languages"), Value::from(book.languages));
    entry.insert(QStringLiteral("rating"), book.rating > 0 ? Value::from(book.rating) : Value::null());
    entry.insert(QStringLiteral("title"), Value::from(book.title));
    entry.insert(QStringLiteral("title_sort"), Value::from(book.sort));
    return entry;
}

QByteArray entryText(const PythonJson::Value &entry)
{
    PythonJson::Format format;
    format.indent = 2;
    format.ensureAscii = true;
    QByteArray text = PythonJson::dumps(entry, format).toUtf8();
    text.replace("\n", "\n  ");
    return text;
}

} // namespace NativeSend
