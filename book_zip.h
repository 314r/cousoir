#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

// A book's ZIP container read whole and written back: an EPUB, a KEPUB.
namespace BookZip {

struct Zip
{
    // The book's files, in their order.
    QStringList names;
    QHash<QString, QByteArray> data;
};

bool readZip(const QString &path, Zip *zip, QString *error);
// The files in their order, the mimetype first and stored, as EPUB wants,
// the others deflated.
bool writeZip(const QString &path, const Zip &zip, QString *error);

} // namespace BookZip
