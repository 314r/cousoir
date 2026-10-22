// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/devices/mtp/driver.py: _sync_kindle_thumbnails() and
//   delete_kindle_cover_thumbnail_for()
// - src/calibre/devices/mtp/filesystem_cache.py and
//   src/calibre/ebooks/__init__.py: the book extensions
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kindle.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <QTemporaryDir>

#include "device_files.h"
#include "kindle_clippings.h"

namespace {

const QString metadataFile = QStringLiteral("metadata.calibre");
const QString driveInfoFile = QStringLiteral("driveinfo.calibre");
const QString coverCache = QStringLiteral("amazon-cover-bug");
const QString thumbnails = QStringLiteral("system/thumbnails");

// BOOK_EXTENSIONS of Calibre 9.16 but those its MTP driver leaves out.
bool isEbook(const QString &name)
{
    static const QSet<QString> extensions{
        QStringLiteral("azw"),     QStringLiteral("azw1"),   QStringLiteral("azw3"),  QStringLiteral("azw4"),
        QStringLiteral("book"),    QStringLiteral("cb7"),    QStringLiteral("cbc"),   QStringLiteral("cbr"),
        QStringLiteral("cbz"),     QStringLiteral("chm"),    QStringLiteral("djv"),   QStringLiteral("djvu"),
        QStringLiteral("doc"),     QStringLiteral("docm"),   QStringLiteral("docx"),  QStringLiteral("epub"),
        QStringLiteral("fb2"),     QStringLiteral("fbz"),    QStringLiteral("htm"),   QStringLiteral("html"),
        QStringLiteral("htmlz"),   QStringLiteral("iba"),    QStringLiteral("ibook"), QStringLiteral("ibooks"),
        QStringLiteral("imp"),     QStringLiteral("kepub"),  QStringLiteral("kfx"),   QStringLiteral("kpf"),
        QStringLiteral("lit"),     QStringLiteral("lrf"),    QStringLiteral("lrx"),   QStringLiteral("markdown"),
        QStringLiteral("md"),      QStringLiteral("mobi"),   QStringLiteral("odt"),   QStringLiteral("oebzip"),
        QStringLiteral("oxps"),    QStringLiteral("pdb"),    QStringLiteral("pdf"),   QStringLiteral("pdr"),
        QStringLiteral("pml"),     QStringLiteral("pmlz"),   QStringLiteral("pobi"),  QStringLiteral("prc"),
        QStringLiteral("ps"),      QStringLiteral("rb"),     QStringLiteral("rtf"),   QStringLiteral("snb"),
        QStringLiteral("text"),    QStringLiteral("textile"), QStringLiteral("tpz"),  QStringLiteral("txt"),
        QStringLiteral("txtz"),    QStringLiteral("updb"),   QStringLiteral("xhtm"),  QStringLiteral("xhtml"),
        QStringLiteral("xps"),     QStringLiteral("zbf"),
    };
    return !name.startsWith(QLatin1String("._")) && extensions.contains(QFileInfo(name).suffix().toLower());
}

// The file at `path` on the device, read whole; empty when it is not there.
QByteArray readWhole(DeviceFiles &files, const QString &path, QString *error)
{
    if (!files.find(path))
        return {};
    QTemporaryDir folder;
    const QString local = folder.filePath(QStringLiteral("file"));
    if (!folder.isValid() || !files.read(path, local, error))
        return {};
    QFile file(local);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeWhole(DeviceFiles &files, const QString &path, const QByteArray &data, QString *error)
{
    QTemporaryDir folder;
    QFile file(folder.filePath(QStringLiteral("file")));
    if (!folder.isValid() || !file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        if (error)
            *error = QObject::tr("Cannot write a temporary file.");
        return false;
    }
    file.close();
    return files.write(path, file.fileName(), error);
}

// recursive_delete(): the file, then each folder it leaves empty, but the
// storage's root.
bool removeUp(DeviceFiles &files, QString path, QString *error)
{
    if (!files.remove(path, error))
        return false;
    for (qsizetype slash = path.lastIndexOf(u'/'); slash > 0; slash = path.lastIndexOf(u'/')) {
        path = path.left(slash);
        if (!files.children(path).isEmpty())
            break;
        if (!files.remove(path, error))
            return false;
    }
    return true;
}

} // namespace

namespace Kindle {

QStringList formatMap(const QString &configDirectory, const QString &serial)
{
    QFile file(QDir(configDirectory).filePath(QStringLiteral("mtp_devices.json")));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject prefs = QJsonDocument::fromJson(file.readAll()).object();
        // get_pref(): the device's own setting, then the device defaults,
        // which for Amazon have a format map the global one gives way to.
        const QJsonArray map = prefs.value(QStringLiteral("device-") + serial).toObject()
                                   .value(QLatin1String("format_map")).toArray();
        if (!map.isEmpty()) {
            QStringList formats;
            for (const QJsonValue &format : map)
                formats.append(format.toString().toLower());
            return formats;
        }
    }
    return { QStringLiteral("azw3"), QStringLiteral("mobi"), QStringLiteral("azw"), QStringLiteral("azw1"),
             QStringLiteral("azw4"), QStringLiteral("kfx"),  QStringLiteral("pdf") };
}

QJsonObject readDriveInfo(DeviceFiles &files)
{
    return QJsonDocument::fromJson(readWhole(files, driveInfoFile, nullptr)).object();
}

QString name(const QJsonObject &driveInfo, const QString &friendlyName, const QString &modelName)
{
    const QString named = driveInfo.value(QLatin1String("device_name")).toString();
    if (!named.isEmpty())
        return named;
    if (!friendlyName.isEmpty())
        return friendlyName;
    return modelName.isEmpty() ? QObject::tr("Kindle") : modelName;
}

QList<DeviceBook> books(DeviceFiles &files, QString *error)
{
    QHash<QString, QJsonObject> sent;
    QString readError;
    const QByteArray raw = readWhole(files, metadataFile, &readError);
    if (!raw.isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
        if (!document.isArray() && error)
            *error = QObject::tr("The device's metadata.calibre is damaged: %1").arg(parseError.errorString());
        for (const QJsonValue &value : document.array()) {
            const QJsonObject entry = value.toObject();
            sent.insert(entry.value(QLatin1String("lpath")).toString().toLower(), entry);
        }
    } else if (!readError.isEmpty() && error) {
        *error = readError;
    }

    QList<DeviceBook> found;
    for (const DeviceFiles::Entry &entry : files.entries()) {
        if (entry.folder || !isEbook(entry.path))
            continue;
        // iterebooks(): not the text files at the root.
        if (!entry.path.contains(u'/') && entry.path.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive))
            continue;
        DeviceBook book;
        book.lpath = entry.path;
        book.size = entry.size;
        book.format = QFileInfo(entry.path).suffix().toUpper();
        const auto known = sent.constFind(entry.path.toLower());
        if (known != sent.cend()) {
            book.sent = true;
            book.title = known->value(QLatin1String("title")).toString();
            for (const QJsonValue &author : known->value(QLatin1String("authors")).toArray())
                book.authors.append(author.toString());
            book.authorSort = known->value(QLatin1String("author_sort")).toString();
            book.uuid = known->value(QLatin1String("uuid")).toString();
            book.applicationId = known->value(QLatin1String("application_id")).toInt();
        } else {
            // As the driver's template, "{title} - {authors}", names files.
            const QString stem = QFileInfo(entry.path).completeBaseName();
            const qsizetype dash = stem.indexOf(QLatin1String(" - "));
            book.title = dash > 0 ? stem.left(dash) : stem;
            if (dash > 0) {
                for (const QString &author : stem.mid(dash + 3).split(QLatin1String(" & ")))
                    book.authors.append(author.trimmed());
            }
        }
        found.append(book);
    }
    return found;
}

QList<DeviceBook> clippings(DeviceFiles &files, QString *error)
{
    const QString path = QStringLiteral("documents/My Clippings.txt");
    QList<DeviceBook> books;
    for (const KindleClippings::Book &clipped : KindleClippings::byBook(KindleClippings::parse(readWhole(files, path, error)))) {
        DeviceBook book;
        book.lpath = path;
        book.title = clipped.title;
        book.authors = clipped.authors;
        book.authorSort = clipped.authorSort;
        // Highlighted, so begun; how far, the Kindle does not write there.
        book.reading.status = 1;
        book.reading.notes = clipped.notes;
        books.append(book);
    }
    return books;
}

void attachClippings(QList<DeviceBook> *books, QList<DeviceBook> *clippings)
{
    for (DeviceBook &book : *books)
        book.reading = {};
    for (qsizetype i = clippings->size() - 1; i >= 0; --i) {
        const DeviceBook &clipped = clippings->at(i);
        if (clipped.bookId <= 0)
            continue;
        const auto on = std::find_if(books->begin(), books->end(),
                                     [&clipped](const DeviceBook &book) { return book.bookId == clipped.bookId; });
        if (on == books->end())
            continue;
        on->reading = clipped.reading;
        clippings->removeAt(i);
    }
}

bool removeBooks(DeviceFiles &files, const QList<DeviceBook> &books, QString *error)
{
    QSet<QString> removed;
    for (const DeviceBook &book : books) {
        if (!files.find(book.lpath))
            continue;
        if (!removeUp(files, book.lpath, error))
            return false;
        // delete_kindle_cover_thumbnail_for(): the cover the book had, found
        // by the index named after its path.
        const QByteArray index
            = QCryptographicHash::hash(book.lpath.toLower().toUtf8(), QCryptographicHash::Sha1).toHex();
        const QString indexPath = coverCache + u'/' + QString::fromLatin1(index);
        const QString thumbnail = QString::fromUtf8(readWhole(files, indexPath, nullptr)).trimmed();
        if (!thumbnail.isEmpty()) {
            if (!files.remove(thumbnails + u'/' + thumbnail, error) || !files.remove(coverCache + u'/' + thumbnail, error)
                || !files.remove(indexPath, error))
                return false;
        }
        removed.insert(book.lpath.toLower());
    }
    if (removed.isEmpty())
        return true;
    const QList<QByteArray> before = DeviceBooks::arrayElements(readWhole(files, metadataFile, error));
    QList<QByteArray> after;
    for (const QByteArray &entry : before) {
        if (!removed.contains(QJsonDocument::fromJson(entry).object().value(QLatin1String("lpath")).toString().toLower()))
            after.append(entry);
    }
    return after.size() == before.size() || writeWhole(files, metadataFile, DeviceBooks::joinElements(after), error);
}

int restoreThumbnails(DeviceFiles &files, QString *error)
{
    if (!files.find(thumbnails) || !files.find(coverCache))
        return 0;
    int count = 0;
    for (const DeviceFiles::Entry &cached : files.children(coverCache)) {
        if (cached.folder)
            continue;
        const QString name = cached.path.mid(cached.path.lastIndexOf(u'/') + 1);
        const DeviceFiles::Entry *shown = files.find(thumbnails + u'/' + name);
        if (!shown || shown->size == cached.size)
            continue;
        const QByteArray cover = readWhole(files, cached.path, error);
        if (cover.isEmpty() || !writeWhole(files, thumbnails + u'/' + name, cover, error))
            return count;
        ++count;
    }
    return count;
}

} // namespace Kindle
