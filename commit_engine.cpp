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

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

#include "calibre_db.h"
#include "calibre_lock.h"
#include "calibre_paths.h"
#include "field_writers.h"
#include "file_transaction.h"
#include "opf.h"
#include "sqlite_db.h"

namespace {

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

bool writeFile(const QString &path, const QByteArray &contents, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(contents) != contents.size()
        || !file.flush()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
#ifdef Q_OS_UNIX
    ::fsync(file.handle());
#endif
    return true;
}

bool hasJournal(const QString &database)
{
    return QFileInfo::exists(database + QStringLiteral("-journal")) || QFileInfo::exists(database + QStringLiteral("-wal"));
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

void syncFolder(const QString &folder)
{
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(folder).constData(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    Q_UNUSED(folder);
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

// format_abspath() without its rename: the file Calibre expects, or one with
// the format's extension.
QString formatFile(const QString &folder, const QString &name, const QString &format)
{
    const QString extension = u'.' + format.toLower();
    const QString expected = QDir(folder).filePath(name + extension);
    if (QFileInfo(expected).isFile())
        return expected;
    const QStringList entries = QDir(folder).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    for (const QString &entry : entries) {
        if (entry.endsWith(extension) && !entry.startsWith(QLatin1String("._")))
            return QDir(folder).filePath(entry);
    }
    return {};
}

// update_path() of backend.py for one book: its folder and files follow its
// title and first author, in the database and on disk.
bool updatePath(SqliteDb &db, FileTransaction &files, const QString &libraryPath, int bookId,
                const QString &unknown, QString *error)
{
    SqliteStatement book(db, QStringLiteral("SELECT title, path FROM books WHERE id=?"));
    book.bind(1, qint64(bookId));
    if (!book.step())
        return true;
    const QString title = book.text(0);
    const QString current = book.text(1);
    SqliteStatement author(db, QStringLiteral("SELECT authors.name FROM books_authors_link link JOIN authors "
                                              "ON authors.id = link.author WHERE link.book=? ORDER BY link.id LIMIT 1"));
    author.bind(1, qint64(bookId));
    const QString firstAuthor = author.step() ? author.text(0).replace(u'|', u',') : unknown;

    QList<QPair<QString, QString>> formats;
    SqliteStatement data(db, QStringLiteral("SELECT format, name FROM data WHERE book=?"));
    data.bind(1, qint64(bookId));
    int extensionLength = 0;
    while (data.step()) {
        formats.append({ data.text(0).toUpper(), data.text(1) });
        extensionLength = std::max(extensionLength, int(data.text(0).size()) + 1);
    }
    if (formats.isEmpty())
        extensionLength = 10;

    const QString path = CalibrePaths::constructPathName(bookId, title, firstAuthor, unknown);
    const QString name = CalibrePaths::constructFileName(bookId, title, firstAuthor, extensionLength, unknown);
    const auto updateDatabase = [&] {
        SqliteStatement names(db, QStringLiteral("UPDATE data SET name=? WHERE book=? AND format=?"));
        for (const auto &format : std::as_const(formats)) {
            names.reset();
            names.bind(1, name);
            names.bind(2, qint64(bookId));
            names.bind(3, format.first);
            names.step();
            if (!names.error().isEmpty()) {
                if (error)
                    *error = names.error();
                return false;
            }
        }
        SqliteStatement setPath(db, QStringLiteral("UPDATE books SET path=? WHERE id=?"));
        setPath.bind(1, path);
        setPath.bind(2, qint64(bookId));
        setPath.step();
        if (!setPath.error().isEmpty()) {
            if (error)
                *error = setPath.error();
            return false;
        }
        return true;
    };

    if (current.isEmpty())
        return updateDatabase();

    const QDir library(libraryPath);
    const QString source = library.filePath(current);
    const QString target = library.filePath(path);
    if (path == current) {
        // Only the files' names may have changed.
        bool changed = false;
        for (const auto &format : std::as_const(formats))
            changed = changed || (!format.second.isEmpty() && format.second != name);
        if (!changed)
            return true;
        for (const auto &format : std::as_const(formats)) {
            const QString file = formatFile(source, format.second, format.first);
            const QString renamed = QDir(source).filePath(name + u'.' + format.first.toLower());
            if (!file.isEmpty() && file != renamed && !files.move(file, renamed, error))
                return false;
        }
        return updateDatabase();
    }

    // Calibre deletes whatever is already at the new place; Reliure only
    // clears an empty folder and refuses otherwise.
    if (QFileInfo::exists(target)) {
        if (!QFileInfo(target).isDir()
            || !QDir(target).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
            if (error)
                *error = QObject::tr("The library already has a folder %1, where Calibre would move this book.").arg(path);
            return false;
        }
        if (!files.remove(target, error))
            return false;
    }

    if (QFileInfo(source).isDir()) {
        // Every entry moves; the format files take the new name.
        QHash<QString, QString> renamedFormats;
        for (const auto &format : std::as_const(formats)) {
            const QString file = formatFile(source, format.second, format.first);
            if (!file.isEmpty())
                renamedFormats.insert(QFileInfo(file).fileName(), name + u'.' + format.first.toLower());
        }
        const QStringList entries = QDir(source).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                           | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            const QString destination = QDir(target).filePath(renamedFormats.value(entry, entry));
            if (!files.move(QDir(source).filePath(entry), destination, error))
                return false;
        }
        files.removeFolderIfEmpty(source);
        files.removeFolderIfEmpty(QFileInfo(source).absolutePath());
    } else if (!files.makeFolder(target, error)) {
        // Nothing to move: Calibre makes the folder all the same.
        return false;
    }
    return updateDatabase();
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
        return fail(QObject::tr("Calibre's interface language, %1, names folders in a way Reliure does not know yet.")
                        .arg(request.config.interfaceLanguage));
    if (!lock.acquire())
        return fail(QObject::tr("Calibre is open on this computer: close it to change the library."));
    if (hasJournal(database))
        return fail(QObject::tr("Calibre is writing to the library. Try again in a moment."));

    const FileIdentity before = CalibreDb::identify(database);
    const QByteArray original = readFile(database, &error);
    if (original.isEmpty() || CalibreDb::identify(database) != before)
        return fail(error.isEmpty() ? QObject::tr("The library changed while it was read. Try again.") : error);

    if (backup && !request.backupFolder.isEmpty()) {
        QDir().mkpath(request.backupFolder);
        const QString copy = QDir(request.backupFolder).filePath(
            QStringLiteral("metadata-%1.db").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
        if (!writeFile(copy, original, &error))
            return fail(error);
        result->backup = copy;
        pruneBackups(request.backupFolder);
    }

    QDir().mkpath(request.cacheFolder);
    QFile::remove(work + QStringLiteral("-journal"));
    if (!writeFile(work, original, &error))
        return fail(error);

    SqliteDb db;
    if (!db.open(work, SqliteDb::Mode::ReadWrite, &error)
        || !CalibreDb::prepareConnection(db, request.config.tweaks, request.config.interfaceLanguage, &error)
        || !db.exec(QStringLiteral("BEGIN IMMEDIATE"), &error))
        return fail(error);

    // The guard checks the structure before; a write never trusts that alone.
    if (!CalibreDb::isKnownSchema(db.value(QStringLiteral("PRAGMA user_version")).toInt(), CalibreDb::schemaFingerprint(db))) {
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        return fail(QObject::tr("The database is from a Calibre version Reliure does not know; nothing was written."));
    }

    FieldWriters::Context context;
    context.tweaks = request.config.tweaks;
    context.interfaceLanguage = request.config.interfaceLanguage;
    context.now = now;
    context.notes = readNotes(request.libraryPath, request.cacheFolder);

    FileTransaction files(journalPath(request.cacheFolder));
    FieldWriters::Result state;
    QSet<int> moved;
    for (const BookEdit &edit : request.edits) {
        const QSet<int> before = std::exchange(state.dirtied, {});
        if (!FieldWriters::apply(db, edit, context, &state, &error)) {
            db.exec(QStringLiteral("ROLLBACK"), nullptr);
            return fail(error);
        }
        // Cache.set_field() moves the files of every book a title or author
        // change touched.
        if (edit.movesFiles())
            moved.unite(state.dirtied);
        state.dirtied.unite(before);
    }
    const QSet<int> dirtied = state.dirtied;
    if (dirtied.isEmpty()) {
        // Nothing changed: the library stays as it is, not even rewritten.
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        db.close();
        QFile::remove(work);
        result->data.libraryPath = QDir::cleanPath(QFileInfo(request.libraryPath).absoluteFilePath());
        return true;
    }
    for (const int book : std::as_const(moved)) {
        if (!updatePath(db, files, request.libraryPath, book, unknown, &error)) {
            db.exec(QStringLiteral("ROLLBACK"), nullptr);
            files.rollback(nullptr);
            return fail(error);
        }
    }
    if (!FieldWriters::markDirtied(db, dirtied, now, &error) || !db.exec(QStringLiteral("COMMIT"), &error)) {
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        files.rollback(nullptr);
        return fail(error);
    }
    const QString check = db.value(QStringLiteral("PRAGMA quick_check")).toString();
    const bool keysOk = !db.value(QStringLiteral("SELECT count(*) FROM pragma_foreign_key_check")).toLongLong();
    if (check != QLatin1String("ok") || !keysOk) {
        files.rollback(nullptr);
        return fail(QObject::tr("The changed database fails its checks (%1); nothing was written.").arg(check));
    }

    // The library as written, for the covers and metadata.opf files.
    LibraryData data;
    data.libraryPath = QDir::cleanPath(QFileInfo(request.libraryPath).absoluteFilePath());
    if (!CalibreDb::readLibrary(db, &data, &error)) {
        files.rollback(nullptr);
        return fail(error);
    }
    db.close();

    const auto bookWithId = [&data](int id) -> const Book * {
        for (const Book &book : std::as_const(data.books)) {
            if (book.id == id)
                return &book;
        }
        return nullptr;
    };
    for (const BookEdit &edit : request.edits) {
        const Book *book = bookWithId(edit.bookId);
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
    const Opf::Settings opf{ request.config.interfaceLanguage, request.producer, request.producerId };
    for (const int id : std::as_const(dirtied)) {
        const Book *book = bookWithId(id);
        if (!book || book->path.isEmpty())
            continue;
        const QString path = QDir(library.filePath(book->path)).filePath(QStringLiteral("metadata.opf"));
        if (!files.write(path, Opf::metadataToOpf(data, *book, opf, readFile(path)), &error)) {
            files.rollback(nullptr);
            return fail(error);
        }
    }

    // Publishing: the copy goes beside metadata.db, then over it, unless
    // metadata.db changed meanwhile.
    const QByteArray written = readFile(work, &error);
    const QByteArray sha256 = QCryptographicHash::hash(written, QCryptographicHash::Sha256);
    const QString incoming = library.filePath(QStringLiteral(".~reliure-%1-metadata.db")
                                                  .arg(QRandomGenerator::global()->generate(), 0, 16));
    if (written.isEmpty() || !files.addScratch(incoming, &error) || !writeFile(incoming, written, &error)) {
        files.rollback(nullptr);
        return fail(error);
    }
    QFile::setPermissions(incoming, QFileInfo(database).permissions());
    copyAttributes(database, incoming);
    if (request.beforePublish)
        request.beforePublish();
    if (CalibreDb::identify(database) != before || hasJournal(database)) {
        files.rollback(nullptr);
        *retry = true;
        return fail(QObject::tr("The library changed while it was being saved."));
    }
    if (!files.aboutToPublish(sha256, &error)
        || std::rename(QFile::encodeName(incoming).constData(), QFile::encodeName(database).constData()) != 0) {
        files.rollback(nullptr);
        return fail(error.isEmpty() ? QObject::tr("Cannot replace %1.").arg(database) : error);
    }
    syncFolder(request.libraryPath);
    files.finish(nullptr);

    // The copy is now what the library reads from.
    const QString snapshot = QDir(request.cacheFolder).filePath(QStringLiteral("snapshot.db"));
    QFile::remove(snapshot);
    QFile::rename(work, snapshot);
    data.source = CalibreDb::identify(database);
    data.sourceSha256 = sha256;
    CalibreDb::findFiles(&data);
    result->data = data;
    result->dirtied = dirtied;
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
            *error = QObject::tr("This is not an image Reliure can read.");
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
