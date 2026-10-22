// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/__init__.py: fit_image()
// - src/calibre/utils/img.py: save_cover_data_to()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "commit_engine.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QObject>
#include <QPainter>
#include <QRandomGenerator>

#include <cmath>
#include <cstdio>

#ifdef Q_OS_UNIX
#include <sys/xattr.h>
#endif

#include "book_operations.h"
#include "calibre_db.h"
#include "calibre_lock.h"
#include "calibre_template.h"
#include "field_writers.h"
#include "file_transaction.h"
#include "opf.h"
#include "opf_reader.h"
#include "sqlite_db.h"

namespace {

// The composite columns' values of the books whose metadata.opf is
// written: found with their files, which templates may read.
void withComposites(LibraryData *data, const QString &interfaceLanguage, const QSet<int> &books)
{
    const bool composites = std::any_of(data->customColumns.cbegin(), data->customColumns.cend(),
                                        [](const CustomColumn &c) { return c.datatype == QLatin1String("composite"); });
    if (!composites)
        return;
    CalibreDb::findFiles(data, &books);
    CalibreTemplate::computeComposites(data, interfaceLanguage, &books);
}

using CommitEngine::Request;
using CommitEngine::Result;

QString journalPath(const QString &cacheFolder)
{
    return QDir(cacheFolder).filePath(QStringLiteral("ops.journal"));
}

QByteArray readFile(const QString &path, QString *error = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return {};
    }
    return file.readAll();
}

// Copies the extended attributes of `from` to `to`: Dropbox keeps its own
// on the files it syncs.
void copyAttributes(const QString &from, const QString &to)
{
#ifdef Q_OS_LINUX
    const QByteArray source = QFile::encodeName(from);
    const QByteArray target = QFile::encodeName(to);
    const ssize_t size = ::listxattr(source.constData(), nullptr, 0);
    if (size <= 0)
        return;
    QByteArray names(size, '\0');
    if (::listxattr(source.constData(), names.data(), size_t(size)) != size)
        return;
    for (const QByteArray &name : names.split('\0')) {
        if (name.isEmpty() || !name.startsWith("user."))
            continue;
        const ssize_t length = ::getxattr(source.constData(), name.constData(), nullptr, 0);
        if (length < 0)
            continue;
        QByteArray value(length, '\0');
        if (::getxattr(source.constData(), name.constData(), value.data(), size_t(length)) == length)
            ::setxattr(target.constData(), name.constData(), value.constData(), size_t(length), 0);
    }
#else
    Q_UNUSED(from);
    Q_UNUSED(to);
#endif
}

// Keeps the newest 30 backups of the folder.
void pruneBackups(const QString &folder)
{
    QDir dir(folder);
    const QStringList backups = dir.entryList({ QStringLiteral("metadata-*.db") }, QDir::Files, QDir::Name | QDir::Reversed);
    for (qsizetype index = 30; index < backups.size(); ++index)
        QFile::remove(dir.filePath(backups.at(index)));
}

// The items with a note in .calnotes/notes.db, read from a copy.
QSet<QPair<QString, int>> readNotes(const QString &libraryPath, const QString &cacheFolder)
{
    QSet<QPair<QString, int>> notes;
    const QString source = QDir(libraryPath).filePath(QStringLiteral(".calnotes/notes.db"));
    if (!QFileInfo::exists(source))
        return notes;
    const QString copy = QDir(cacheFolder).filePath(QStringLiteral("notes.db"));
    QFile::remove(copy);
    if (!QFile::copy(source, copy))
        return notes;
    SqliteDb db;
    if (db.open(copy, SqliteDb::Mode::Immutable, nullptr)) {
        SqliteStatement statement(db, QStringLiteral("SELECT colname, item FROM notes"));
        while (statement.step())
            notes.insert({ statement.text(0), int(statement.integer(1)) });
    }
    db.close();
    QFile::remove(copy);
    return notes;
}

// One attempt: false with `retry` when metadata.db changed under it.
bool attempt(const Request &request, CalibreLock &lock, Result *result, bool backup, bool *retry)
{
    *retry = false;
    const QDir library(request.libraryPath);
    const QString database = library.filePath(QStringLiteral("metadata.db"));
    const QString work = QDir(request.cacheFolder).filePath(QStringLiteral("work.db"));
    const QString unknown = CalibreText::unknownText(request.config.interfaceLanguage);
    const QDateTime now = request.now.isValid() ? request.now : QDateTime::currentDateTimeUtc();
    QString error;
    const auto fail = [&](const QString &message) {
        result->error = message;
        return false;
    };

    // Calibre names folders with another table in these languages.
    if (!CalibreText::hasAsciiTable(request.config.interfaceLanguage))
        return fail(QObject::tr("Calibre's interface language, %1, names folders in a way Cousoir does not know yet.")
                        .arg(request.config.interfaceLanguage));
    if (!lock.acquire())
        return fail(QObject::tr("Calibre is open on this computer: close it to change the library."));
    if (CalibreDb::hasJournal(database))
        return fail(QObject::tr("Calibre is writing to the library. Try again in a moment."));

    const FileIdentity before = CalibreDb::identify(database);
    const QByteArray original = readFile(database, &error);
    if (original.isEmpty() || CalibreDb::identify(database) != before)
        return fail(error.isEmpty() ? QObject::tr("The library changed while it was read. Try again.") : error);

    if (backup && !request.backupFolder.isEmpty()) {
        QDir().mkpath(request.backupFolder);
        const QString copy = QDir(request.backupFolder).filePath(
            QStringLiteral("metadata-%1.db").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
        if (!FileTransaction::writeDurably(copy, original, &error))
            return fail(error);
        result->backup = copy;
        pruneBackups(request.backupFolder);
    }

    QDir().mkpath(request.cacheFolder);
    QFile::remove(work + QStringLiteral("-journal"));
    if (!FileTransaction::writeDurably(work, original, &error))
        return fail(error);

    SqliteDb db;
    if (!db.open(work, SqliteDb::Mode::ReadWrite, &error)
        || !CalibreDb::prepareConnection(db, request.config.tweaks, request.config.interfaceLanguage, &error)
        || !db.exec(QStringLiteral("BEGIN IMMEDIATE"), &error))
        return fail(error);

    // The guard checks the structure before; a write never trusts that alone.
    if (!CalibreDb::isKnownSchema(db.value(QStringLiteral("PRAGMA user_version")).toInt(), CalibreDb::schemaFingerprint(db))) {
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        return fail(QObject::tr("The database is from a Calibre version Cousoir does not know; nothing was written."));
    }

    FieldWriters::Context context;
    context.tweaks = request.config.tweaks;
    context.interfaceLanguage = request.config.interfaceLanguage;
    context.now = now;
    context.notes = readNotes(request.libraryPath, request.cacheFolder);

    FileTransaction files(journalPath(request.cacheFolder));
    FieldWriters::Result state;
    QSet<int> moved;
    const auto undo = [&](const QString &message) {
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        files.rollback(nullptr);
        return fail(message);
    };
    const auto applyEdit = [&](const BookEdit &edit) {
        const QSet<int> dirtiedBefore = std::exchange(state.dirtied, {});
        if (!FieldWriters::apply(db, edit, context, &state, &error))
            return false;
        // Cache.set_field() moves the files of every book a title or author
        // change touched.
        if (edit.movesFiles())
            moved.unite(state.dirtied);
        state.dirtied.unite(dirtiedBefore);
        return true;
    };
    for (const BookEdit &edit : request.edits) {
        if (!applyEdit(edit))
            return undo(error);
    }
    for (const BulkEdit &bulk : request.bulkEdits) {
        LibraryData current;
        current.libraryPath = QDir::cleanPath(QFileInfo(request.libraryPath).absoluteFilePath());
        if (!CalibreDb::readLibrary(db, &current, &error))
            return undo(error);
        for (const int id : bulk.bookIds) {
            const Book *book = current.book(id);
            const bool kept = std::any_of(result->bulkBefore.cbegin(), result->bulkBefore.cend(),
                                          [id](const Book &before) { return before.id == id; });
            if (book && !kept)
                result->bulkBefore.append(*book);
        }
        for (const BookEdit &edit : bulk.edits(current, request.config.tweaks)) {
            if (!applyEdit(edit))
                return undo(error);
        }
    }
    for (const int book : std::as_const(moved)) {
        if (!BookOperations::updatePath(db, files, request.libraryPath, book, unknown, &error))
            return undo(error);
    }

    // Formats copied, then formats and books removed, restored and added, in
    // that order, then the settings of the conversions that added formats.
    const Opf::Settings opf{ request.config.interfaceLanguage, request.producer, request.producerId };
    BookOperations::Context operations{ request.libraryPath, context, request.config.newBookTags, nullptr, opf };
    BookOperations::Result done;
    for (const FormatCopy &copy : request.formatCopies) {
        if (!BookOperations::copyFormats(db, files, copy, operations, &done, &error))
            return undo(error);
    }
    for (const FormatRemoval &removal : request.formatRemovals) {
        if (!BookOperations::removeFormats(db, files, removal, operations, &done, &error))
            return undo(error);
    }
    LibraryData trashed;
    if (!request.trash.isEmpty()) {
        // The books as they are now, for their metadata.opf.
        trashed.libraryPath = QDir::cleanPath(QFileInfo(request.libraryPath).absoluteFilePath());
        if (!CalibreDb::readLibrary(db, &trashed, &error))
            return undo(error);
        withComposites(&trashed, request.config.interfaceLanguage,
                       QSet<int>(request.trash.cbegin(), request.trash.cend()));
        operations.before = &trashed;
        if (!BookOperations::removeBooks(db, files, request.trash, operations, &done, &error))
            return undo(error);
        operations.before = nullptr;
    }
    for (const Restoration &restoration : request.restorations) {
        if (!BookOperations::restore(db, files, restoration, operations, &state, &done, &error))
            return undo(error);
    }
    for (const NewBook &book : request.newBooks) {
        if (!BookOperations::addBook(db, files, book, operations, &state, &done, &error))
            return undo(error);
    }
    for (const NewFormat &format : request.newFormats) {
        if (!BookOperations::addFormat(db, files, format, operations, &done, &error))
            return undo(error);
    }
    for (const auto &options : request.conversionOptions) {
        if (!BookOperations::setConversionOptions(db, options.first, options.second, &error))
            return undo(error);
    }

    QSet<int> dirtied = state.dirtied;
    dirtied.unite(done.dirtied);
    const QSet<int> touched = done.touched - dirtied;
    const bool operated = request.hasOperations();
    // Books removed are no longer there to write.
    for (const int book : request.trash)
        dirtied.remove(book);
    if (dirtied.isEmpty() && !operated) {
        // Nothing changed: the library stays as it is, not even rewritten.
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        db.close();
        QFile::remove(work);
        result->data.libraryPath = QDir::cleanPath(QFileInfo(request.libraryPath).absoluteFilePath());
        return true;
    }
    if (!FieldWriters::markDirtied(db, dirtied, now, &error) || !FieldWriters::touch(db, touched, now, &error)
        || !db.exec(QStringLiteral("COMMIT"), &error))
        return undo(error);
    const QString check = db.value(QStringLiteral("PRAGMA quick_check")).toString();
    const bool keysOk = !db.value(QStringLiteral("SELECT count(*) FROM pragma_foreign_key_check")).toLongLong();
    if (check != QLatin1String("ok") || !keysOk) {
        files.rollback(nullptr);
        return fail(QObject::tr("The changed database fails its checks (%1); nothing was written.").arg(check));
    }

    // The library as written, for the covers and metadata.opf files; the
    // dates of the books added and restored as Calibre holds them then.
    LibraryData data;
    data.libraryPath = QDir::cleanPath(QFileInfo(request.libraryPath).absoluteFilePath());
    if (!CalibreDb::readLibrary(db, &data, &error)) {
        files.rollback(nullptr);
        return fail(error);
    }
    withComposites(&data, request.config.interfaceLanguage, dirtied);
    QHash<int, Opf::ExactDates> exactDates;
    for (const int id : done.added + done.restored) {
        SqliteStatement dates(db, QStringLiteral("SELECT pubdate, timestamp FROM books WHERE id=?"));
        dates.bind(1, qint64(id));
        if (dates.step()) {
            exactDates.insert(id, { OpfReader::parseDate(dates.text(0), true).value_or(CalibreDate()),
                                    OpfReader::parseDate(dates.text(1), true).value_or(CalibreDate()) });
        }
    }
    db.close();

    for (const BookEdit &edit : request.edits) {
        const Book *book = data.book(edit.bookId);
        if (!edit.cover || !book || book->path.isEmpty())
            continue;
        const QString cover = QDir(library.filePath(book->path)).filePath(QStringLiteral("cover.jpg"));
        if (edit.cover->isEmpty()) {
            if (!files.remove(cover, &error)) {
                files.rollback(nullptr);
                return fail(error);
            }
            continue;
        }
        const QByteArray image = CommitEngine::coverData(*edit.cover, &error);
        if (image.isEmpty() || !files.write(cover, image, &error)) {
            files.rollback(nullptr);
            return fail(error);
        }
    }
    for (const int id : std::as_const(dirtied)) {
        const Book *book = data.book(id);
        if (!book || book->path.isEmpty())
            continue;
        const QString path = QDir(library.filePath(book->path)).filePath(QStringLiteral("metadata.opf"));
        const auto exact = exactDates.constFind(id);
        if (!files.write(path, Opf::metadataToOpf(data, *book, opf, readFile(path),
                                                  exact != exactDates.cend() ? &*exact : nullptr), &error)) {
            files.rollback(nullptr);
            return fail(error);
        }
    }

    // Publishing: the copy goes beside metadata.db, then over it, unless
    // metadata.db changed meanwhile.
    const QByteArray written = readFile(work, &error);
    const QByteArray sha256 = QCryptographicHash::hash(written, QCryptographicHash::Sha256);
    const QString incoming = library.filePath(QStringLiteral(".~cousoir-%1-metadata.db")
                                                  .arg(QRandomGenerator::global()->generate(), 0, 16));
    if (written.isEmpty() || !files.addScratch(incoming, &error) || !FileTransaction::writeDurably(incoming, written, &error)) {
        files.rollback(nullptr);
        return fail(error);
    }
    QFile::setPermissions(incoming, QFileInfo(database).permissions());
    copyAttributes(database, incoming);
    if (request.beforePublish)
        request.beforePublish();
    if (CalibreDb::identify(database) != before || CalibreDb::hasJournal(database)) {
        files.rollback(nullptr);
        *retry = true;
        return fail(QObject::tr("The library changed while it was being saved."));
    }
    if (!files.aboutToPublish(sha256, &error)
        || std::rename(QFile::encodeName(incoming).constData(), QFile::encodeName(database).constData()) != 0) {
        files.rollback(nullptr);
        return fail(error.isEmpty() ? QObject::tr("Cannot replace %1.").arg(database) : error);
    }
    FileTransaction::sync(request.libraryPath);
    files.finish();

    // The copy is now what the library reads from.
    const QString snapshot = QDir(request.cacheFolder).filePath(QStringLiteral("snapshot.db"));
    QFile::remove(snapshot);
    QFile::rename(work, snapshot);
    data.source = CalibreDb::identify(database);
    CalibreDb::findFiles(&data);
    result->data = data;
    result->written = true;
    result->dirtied = dirtied;
    result->added = done.added;
    return true;
}

} // namespace

namespace CommitEngine {

Result commit(const Request &request)
{
    Result result;
    QString done;
    if (!recover(request.libraryPath, request.cacheFolder, &done, &result.error))
        return result;
    CalibreLock lock;
    bool retry = false;
    for (int round = 0; round < 3; ++round) {
        result = Result();
        result.ok = attempt(request, lock, &result, round == 0, &retry);
        if (result.ok || !retry)
            break;
    }
    return result;
}

bool recover(const QString &libraryPath, const QString &cacheFolder, QString *done, QString *error)
{
    return FileTransaction::recover(journalPath(cacheFolder), QDir(libraryPath).filePath(QStringLiteral("metadata.db")),
                                    done, error);
}

QByteArray coverData(const QByteArray &data, QString *error)
{
    QBuffer buffer;
    buffer.setData(data);
    QImageReader reader(&buffer);
    const QByteArray format = reader.format().toLower();
    QImage image;
    if (!reader.read(&image)) {
        if (error)
            *error = QObject::tr("This is not an image Cousoir can read.");
        return {};
    }
    // fit_image() into maximum_cover_size.
    constexpr int maxWidth = 1650;
    constexpr int maxHeight = 2200;
    int width = image.width();
    int height = image.height();
    const bool scaled = width > maxWidth || height > maxHeight;
    if (height > maxHeight) {
        width = int(std::floor(double(maxHeight) / height * width));
        height = maxHeight;
    }
    if (width > maxWidth) {
        height = int(std::floor(double(maxWidth) / width * height));
        width = maxWidth;
    }
    if (height > maxHeight) {
        width = int(std::floor(double(maxHeight) / height * width));
        height = maxHeight;
    }
    const bool jpeg = format == "jpeg" || format == "jpg";
    if (jpeg && !scaled && !image.hasAlphaChannel())
        return data;
    if (scaled)
        image = image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.hasAlphaChannel()) {
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        painter.end();
        image = flat;
    }
    QByteArray encoded;
    QBuffer out(&encoded);
    out.open(QIODevice::WriteOnly);
    QImageWriter writer(&out, "jpeg");
    writer.setQuality(90);
    if (!writer.write(image)) {
        if (error)
            *error = writer.errorString();
        return {};
    }
    return encoded;
}

} // namespace CommitEngine
