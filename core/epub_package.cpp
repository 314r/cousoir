// SPDX-License-Identifier: GPL-3.0-only

#include "epub_package.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QUrl>

#include <zip.h>

#include "xml_tree.h"

namespace EpubPackage {

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
        zip_file_t *file = zip_fopen_index(archive, zip_uint64_t(i), 0);
        if (!file)
            continue;
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

QString relativeHref(const QString &target, const QString &from)
{
    const QString base = from.contains(u'/') ? from.section(u'/', 0, -2) : QString();
    const QString relative = QDir(QStringLiteral("/") + base).relativeFilePath(QStringLiteral("/") + target);
    return QString::fromUtf8(QUrl::toPercentEncoding(relative, "/-._~"));
}

QString freeName(const Zip &zip, const QString &name)
{
    if (!zip.data.contains(name))
        return name;
    const QString base = name.section(u'.', 0, -2);
    const QString extension = name.section(u'.', -1);
    for (int i = 1;; ++i) {
        const QString candidate = QStringLiteral("%1-%2.%3").arg(base).arg(i).arg(extension);
        if (!zip.data.contains(candidate))
            return candidate;
    }
}

QString packageName(const Zip &zip)
{
    const std::unique_ptr<XmlTree::Node> container = XmlTree::parse(zip.data.value(QStringLiteral("META-INF/container.xml")));
    if (!container)
        return {};
    // The first package there.
    for (const XmlTree::Node *rootfile :
         XmlTree::findAll(container.get(), [](const XmlTree::Node &n) { return n.isElement("rootfile"); })) {
        const QString name = rootfile->attribute(QStringLiteral("full-path"));
        if (zip.data.contains(name))
            return name;
    }
    return {};
}

QString resolve(const QString &href, const QString &from)
{
    const QString path = href.section(u'#', 0, 0).section(u'?', 0, 0);
    // A scheme: an address elsewhere.
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (scheme.match(path).hasMatch())
        return {};
    if (path.isEmpty())
        return href.startsWith(u'#') ? from : QString();
    const QString decoded = QUrl::fromPercentEncoding(path.toUtf8());
    if (decoded.startsWith(u'/'))
        return QDir::cleanPath(decoded.mid(1));
    const QString base = from.contains(u'/') ? from.section(u'/', 0, -2) + u'/' : QString();
    const QString name = QDir::cleanPath(base + decoded);
    return name.startsWith(QLatin1String("../")) ? QString() : name;
}

} // namespace EpubPackage
