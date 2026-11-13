#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

// An EPUB's files, as Cousoir's conversions of an EPUB read and write them:
// all of them in memory, by their names from the archive's root.
namespace EpubPackage {

struct Zip
{
    // The book's files, in their order.
    QStringList names;
    QHash<QString, QByteArray> data;
};

bool readZip(const QString &path, Zip *zip, QString *error);
// The mimetype first and stored, as EPUB wants.
bool writeZip(const QString &path, const Zip &zip, QString *error);
// The package document's name, by META-INF/container.xml; empty for none.
QString packageName(const Zip &zip);
// A name's address from another name of the book, both from its root.
QString relativeHref(const QString &target, const QString &from);
// The name an address of the file `from` leads to, without its fragment;
// empty for an address elsewhere ("http:", "data:"...).
QString resolve(const QString &href, const QString &from);
// A name not taken yet: `name`, else name-1, name-2...
QString freeName(const Zip &zip, const QString &name);

} // namespace EpubPackage
