#include "save_to_disk.h"

// Ported from Calibre 9.17, src/calibre/library/save_to_disk.py: config(),
// preprocess_template(), get_components(), get_formats(),
// do_save_book_to_disk(), sanitize_args(). Copyright (C) Kovid Goyal, GPL v3.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include "calibre_text.h"
#include "native_send.h"

namespace SaveToDisk {

namespace {

// preprocess_template().
QString preprocessed(QString text)
{
    return text.replace(QStringLiteral("//"), QStringLiteral("/"))
        .replace(QStringLiteral("{author}"), QStringLiteral("{authors}"))
        .replace(QStringLiteral("{tag}"), QStringLiteral("{tags}"));
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

bool copyFile(const QString &from, const QString &to)
{
    QFile::remove(to);
    return QFile::copy(from, to);
}

} // namespace

Options Options::load(const QString &calibreConfigDirectory)
{
    Options options;
    QFile file(QDir(calibreConfigDirectory).filePath(QStringLiteral("save_to_disk.py.json")));
    if (!file.open(QIODevice::ReadOnly))
        return options;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    const auto text = [&json](const char *key, QString *value) {
        if (json.value(QLatin1String(key)).isString())
            *value = json.value(QLatin1String(key)).toString();
    };
    const auto flag = [&json](const char *key, bool *value) {
        if (json.value(QLatin1String(key)).isBool())
            *value = json.value(QLatin1String(key)).toBool();
    };
    text("template", &options.templateText);
    text("formats", &options.formats);
    flag("asciiize", &options.asciiize);
    flag("to_lowercase", &options.toLowercase);
    flag("replace_whitespace", &options.replaceWhitespace);
    flag("single_dir", &options.singleDir);
    flag("save_cover", &options.saveCover);
    flag("write_opf", &options.writeOpf);
    return options;
}

QStringList components(const LibraryData &data, const Book &book, const Options &options, qsizetype length,
                       const QString &interfaceLanguage)
{
    const QString formatted = NativeSend::componentText(data, book, preprocessed(options.templateText), interfaceLanguage);
    static const QRegularExpression space(QStringLiteral("\\s"));
    QStringList parts;
    for (const QString &part : formatted.split(u'/')) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty())
            continue;
        QString clean = options.asciiize ? CalibreText::asciiFilename(trimmed) : CalibreText::sanitizeFileName(trimmed);
        if (options.toLowercase)
            clean = clean.toLower();
        if (options.replaceWhitespace)
            clean.replace(space, QStringLiteral("_"));
        parts.append(clean);
    }
    if (parts.isEmpty())
        parts.append(QString::number(book.id));
    if (options.singleDir)
        parts = { parts.last() };
    return NativeSend::shortenComponentsTo(length, parts, false);
}

Outcome save(const LibraryData &data, const QList<int> &bookIds, const QString &root, const Options &options,
             const Opf::Settings &opf, const std::function<bool(int)> &progress)
{
    Outcome outcome;
    const QString base = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    // sanitize_args(): what the paths under the root may take.
    const qsizetype length = 1024 - 15 - base.size();
    if (length < 5) {
        outcome.failures.append(QObject::tr("%1 is too long a path to save books under.").arg(base));
        return outcome;
    }
    // get_formats(): those asked for that the book has.
    QSet<QString> asked;
    for (const QString &format : options.formats.split(u',', Qt::SkipEmptyParts))
        asked.insert(format.trimmed().toLower());
    int done = 0;
    for (const int id : bookIds) {
        const Book *book = data.book(id);
        if (!book)
            continue;
        const QStringList parts = components(data, *book, options, length, opf.interfaceLanguage);
        const QString path = QDir(base).filePath(parts.join(u'/'));
        const QString name = QFileInfo(path).fileName();
        QString why;
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            why = QObject::tr("cannot create the folder %1").arg(QFileInfo(path).absolutePath());
        bool coverSaved = false;
        if (why.isEmpty() && options.saveCover && !book->coverFile.isEmpty()) {
            coverSaved = copyFile(book->coverFile, path + QStringLiteral(".jpg"));
            if (!coverSaved)
                why = QObject::tr("cannot write its cover");
        }
        if (why.isEmpty() && options.writeOpf) {
            Opf::Settings settings = opf;
            settings.applicationId = QString::number(book->id);
            settings.coverReference = coverSaved;
            settings.coverHref = name + QStringLiteral(".jpg");
            if (!writeFile(path + QStringLiteral(".opf"), Opf::metadataToOpf(data, *book, settings)))
                why = QObject::tr("cannot write its OPF");
        }
        bool written = false;
        if (why.isEmpty()) {
            for (const BookFormat &format : book->formats) {
                const QString lower = format.format.toLower();
                if (options.formats.trimmed() != QLatin1String("all") && !asked.contains(lower))
                    continue;
                if (format.file.isEmpty() || !QFileInfo::exists(format.file))
                    continue;
                if (!copyFile(format.file, path + u'.' + lower)) {
                    why = QObject::tr("cannot copy its %1 file").arg(format.format);
                    break;
                }
                written = true;
            }
            if (why.isEmpty() && !written)
                why = QObject::tr("it has none of the formats asked for");
        }
        if (why.isEmpty())
            ++outcome.saved;
        else
            outcome.failures.append(QStringLiteral("%1: %2").arg(book->title, why));
        if (progress && !progress(++done))
            break;
    }
    return outcome;
}

} // namespace SaveToDisk
