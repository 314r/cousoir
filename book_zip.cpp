#include "book_zip.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <zip.h>

namespace BookZip {

bool readZip(const QString &path, Zip *zip, QString *error)
{
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &code);
    if (!archive) {
        *error = QObject::tr("%1 is not a ZIP archive.").arg(QFileInfo(path).fileName());
        return false;
    }
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive, zip_uint64_t(i), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_NAME))
            continue;
        const QString name = QString::fromUtf8(stat.name);
        if (name.endsWith(u'/'))
            continue;
        // A file that cannot be read fails the book rather than go missing
        // from what is written back.
        zip_file_t *file = zip_fopen_index(archive, zip_uint64_t(i), 0);
        if (!file) {
            zip_discard(archive);
            *error = QObject::tr("Cannot read %1 in %2.").arg(name, QFileInfo(path).fileName());
            return false;
        }
        QByteArray bytes(qsizetype(stat.size), Qt::Uninitialized);
        const zip_int64_t read = zip_fread(file, bytes.data(), stat.size);
        zip_fclose(file);
        if (read < 0 || zip_uint64_t(read) != stat.size) {
            zip_discard(archive);
            *error = QObject::tr("Cannot read %1 in %2.").arg(name, QFileInfo(path).fileName());
            return false;
        }
        zip->names.append(name);
        zip->data.insert(name, bytes);
    }
    zip_discard(archive);
    return true;
}

bool writeZip(const QString &path, const Zip &zip, QString *error)
{
    QFile::remove(path);
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &code);
    if (!archive) {
        *error = QObject::tr("Cannot write %1.").arg(path);
        return false;
    }
    // The mimetype first and stored, as EPUB wants.
    QStringList names = zip.names;
    if (names.removeAll(QStringLiteral("mimetype")) > 0)
        names.prepend(QStringLiteral("mimetype"));
    for (const QString &name : std::as_const(names)) {
        const QByteArray &bytes = zip.data[name];
        zip_source_t *source = zip_source_buffer(archive, bytes.constData(), zip_uint64_t(bytes.size()), 0);
        const zip_int64_t index = source ? zip_file_add(archive, name.toUtf8().constData(), source, ZIP_FL_ENC_UTF_8) : -1;
        if (index < 0) {
            if (source)
                zip_source_free(source);
            zip_discard(archive);
            *error = QObject::tr("Cannot write %1.").arg(path);
            return false;
        }
        const bool store = name == QLatin1String("mimetype");
        zip_set_file_compression(archive, zip_uint64_t(index), store ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0);
    }
    if (zip_close(archive) != 0) {
        *error = QObject::tr("Cannot write %1: %2").arg(path, QString::fromUtf8(zip_strerror(archive)));
        zip_discard(archive);
        return false;
    }
    return true;
}

} // namespace BookZip
