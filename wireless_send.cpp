#include "wireless_send.h"

// Ported from Calibre 9.16, Copyright (C) Kovid Goyal and Charles Haley,
// GPL v3:
// - src/calibre/devices/smart_device_app/driver.py: _create_upload_path(),
//   _json_encode()'s _series_sort_, SDBook and Book's mime;
// - src/calibre/gui2/device.py: sync_to_device()'s choice of format,
//   cover_to_thumbnail(), set_books_in_library() with
//   manage_device_metadata 'on_connect'.

#include <QBuffer>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include "book.h"
#include "calibre_text.h"
#include "native_send.h"

using PythonJson::Value;

namespace WirelessSend {

namespace {

QDateTime parseDate(const QString &text)
{
    if (text.isEmpty() || text == QLatin1String("None"))
        return {};
    return QDateTime::fromString(text, Qt::ISODateWithMs);
}

int thumbnailHeight(const Value &book)
{
    const Value *thumbnail = book.find(QStringLiteral("thumbnail"));
    if (!thumbnail || thumbnail->kind != Value::Kind::List || thumbnail->items.size() < 2)
        return -1;
    return int(std::max(thumbnail->items.at(0).integer, thumbnail->items.at(1).integer));
}

// update_thumbnail() for a book without a cover: Calibre draws one, its
// default cover's 600 by 800 scaled to the height asked for; Cousoir draws
// the title and authors on a colour of the title's, where it can draw text.
QImage drawnCover(const Book &book, int height)
{
    const int width = int(600 * (double(height) / 800));
    QImage image(width, height, QImage::Format_RGB32);
    const QColor colour = QColor::fromHsl(int(qHash(book.title) % 360), 90, 70);
    image.fill(colour);
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        QPainter painter(&image);
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPixelSize(std::max(8, height / 14));
        font.setBold(true);
        painter.setFont(font);
        const QRect area = image.rect().adjusted(width / 10, height / 8, -width / 10, -height / 8);
        painter.drawText(area, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, book.title);
        font.setBold(false);
        font.setPixelSize(std::max(7, height / 20));
        painter.setFont(font);
        painter.drawText(area, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, book.authors.join(QStringLiteral(" & ")));
    }
    return image;
}

Value jpegValue(const QImage &image, int quality)
{
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", quality);
    Value value;
    value.kind = Value::Kind::List;
    value.items = { Value::from(image.width()), Value::from(image.height()),
                    Value::from(QString::fromLatin1(jpeg.toBase64())) };
    return value;
}

// cover_to_thumbnail(): within the height asked for, both ways, at the
// driver's quality; or exactly the size asked for, when the app gives a
// width too.
Value thumbnail(const Book &book, const Wireless::App &app)
{
    const QImage cover = book.coverFile.isEmpty() ? QImage() : QImage(book.coverFile);
    if (cover.isNull())
        return jpegValue(drawnCover(book, app.coverHeight), 75);
    QImage image;
    int quality = 75;
    if (app.coverWidth) {
        image = cover.scaled(*app.coverWidth, app.coverHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        quality = 70;
    } else {
        image = NativeSend::thumbnailImage(cover, app.coverHeight, app.coverHeight);
    }
    return jpegValue(image, quality);
}

} // namespace

QString format(const Book &book, const Wireless::App &app)
{
    QStringList formats;
    for (const BookFormat &bookFormat : book.formats) {
        if (!bookFormat.file.isEmpty())
            formats.append(bookFormat.format.toUpper());
    }
    return DeviceBooks::sendFormat(formats, app.acceptedExtensions);
}

QString uploadPath(const LibraryData &data, const Book &book, const QString &extension, const Wireless::App &app,
                   const QString &interfaceLanguage)
{
    if (app.useUuidFileNames && !book.uuid.isEmpty())
        return book.uuid + extension;
    // MAX_PATH_LEN less PATH_FUDGE_FACTOR and what the app keeps for the
    // extension.
    constexpr int maxPathLength = 250;
    constexpr int fudge = 40;
    NativeSend::UploadOptions options;
    options.saveTemplate = QStringLiteral("{title} - {authors} ({id})");
    options.maxLength = maxPathLength - (fudge + app.extensionPathLengths.value(extension.mid(1).toLower(), fudge));
    options.useSubdirs = true;
    return NativeSend::uploadPath(data, book, extension, options, interfaceLanguage);
}

Value metadata(const LibraryData &data, const Book &book, const QString &lpath, qint64 size, const Wireless::App &app,
               const QString &interfaceLanguage)
{
    Value entry = NativeSend::bookEntry(data, book, lpath, size, thumbnail(book, app));
    const QString extension = QFileInfo(lpath).suffix().toLower();
    const QString mime = Wireless::tables().mimeTypes.value(extension);
    entry.insert(QStringLiteral("mime"), mime.isEmpty() ? Value::null() : Value::from(mime));
    entry.insert(QStringLiteral("cover"), book.coverFile.isEmpty() ? Value::null() : Value::from(book.coverFile));
    // _json_encode(): the series sorted as save templates sort it.
    const QString seriesSort = book.series.isEmpty()
        ? QString()
        : CalibreText::titleSort(book.series, QString(), CalibreText::Tweaks::defaults(), interfaceLanguage,
                                 QStringLiteral("library_order"));
    entry.insert(QStringLiteral("_series_sort_"), Value::from(seriesSort));
    return entry;
}

QList<DeviceBook> deviceBooks(const QList<Value> &books, const LibraryData &data)
{
    QList<DeviceBook> result;
    for (const Value &book : books) {
        DeviceBook device;
        device.lpath = book.text(QStringLiteral("lpath"));
        device.title = book.text(QStringLiteral("title"));
        if (const Value *authors = book.find(QStringLiteral("authors"))) {
            for (const Value &author : authors->items)
                device.authors.append(author.string);
        }
        device.authorSort = book.text(QStringLiteral("author_sort"));
        device.uuid = book.text(QStringLiteral("uuid"));
        if (const Value *id = book.find(QStringLiteral("application_id")))
            device.applicationId = int(id->integer);
        if (const Value *size = book.find(QStringLiteral("size")))
            device.size = size->kind == Value::Kind::Float ? qint64(size->real) : size->integer;
        device.format = QFileInfo(device.lpath).suffix().toUpper();
        device.sent = !device.uuid.isEmpty();
        if (device.title.isEmpty())
            device.title = QFileInfo(device.lpath).completeBaseName();
        result.append(device);
    }
    DeviceBooks::match(&result, data);
    return result;
}

QList<Value> changedMetadata(const QList<Value> &books, const LibraryData &data, const Wireless::App &app,
                             const QString &interfaceLanguage)
{
    const QList<DeviceBook> matched = deviceBooks(books, data);
    QList<Value> changed;
    for (qsizetype i = 0; i < books.size(); ++i) {
        const Book *book = matched.at(i).bookId ? data.book(matched.at(i).bookId) : nullptr;
        if (!book)
            continue;
        const Value &onApp = books.at(i);
        // updateq(): a book matched by its uuid is updated when the library
        // changed it since, or its thumbnail is not the height asked for; one
        // matched by its title, always.
        if (book->uuid == onApp.text(QStringLiteral("uuid"))) {
            const int height = thumbnailHeight(onApp);
            const bool modified = parseDate(NativeSend::isoDate(book->lastModified).string)
                != parseDate(onApp.text(QStringLiteral("last_modified")));
            if (!modified && (height < 0 || height == app.coverHeight))
                continue;
        }
        // smart_update() with the library's metadata, which has no type.
        Value updated = metadata(data, *book, onApp.text(QStringLiteral("lpath")), matched.at(i).size, app,
                                 interfaceLanguage);
        updated.insert(QStringLiteral("mime"), Value::null());
        changed.append(updated);
    }
    return changed;
}

} // namespace WirelessSend
