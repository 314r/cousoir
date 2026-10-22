#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "device_books.h"

class DeviceFiles;

// A Kindle reached over MTP, as Calibre 9.16's MTP driver handles one: the
// books in its documents folder, metadata.calibre and driveinfo.calibre at
// its root, and the covers of the books sent, which Amazon replaces with
// blank ones once the Kindle syncs, kept in amazon-cover-bug to put back.
namespace Kindle {

// Amazon's USB vendor id.
constexpr quint16 VendorId = 0x1949;

// The formats Calibre's MTP driver sends a Kindle, in the order it prefers
// them, unless its settings for the device, in mtp_devices.json of
// `configDirectory`, say otherwise.
QStringList formatMap(const QString &configDirectory, const QString &serial);

// driveinfo.calibre, read through `files`; empty when there is none.
QJsonObject readDriveInfo(DeviceFiles &files);
// The name Calibre gave the Kindle in driveinfo.calibre, else the one the
// Kindle gives itself, else its model's.
QString name(const QJsonObject &driveInfo, const QString &friendlyName, const QString &modelName);

// The books on the Kindle as MTP_DEVICE.books() lists them: every e-book
// file but text files at the root, with the metadata Calibre keeps for those
// it sent. Calibre reads the others' metadata from their files; Cousoir
// shows their names.
QList<DeviceBook> books(DeviceFiles &files, QString *error);

// Removes books as MTP_DEVICE.delete_books() and
// remove_books_from_metadata() do: each file and the folders it leaves
// empty, its cover kept against Amazon's bug, then its entry in
// metadata.calibre, whose others stay as written.
bool removeBooks(DeviceFiles &files, const QList<DeviceBook> &books, QString *error);

// The books of documents/My Clippings.txt, each with its highlights, notes
// and bookmarks, as books of the Kindle at that file's path; none when
// there is no such file.
QList<DeviceBook> clippings(DeviceFiles &files, QString *error);

// Gives the Kindle's books the notes of `clippings` of the same library
// book, matched both; the others' stay in `clippings`, for books gone from
// the Kindle.
void attachClippings(QList<DeviceBook> *books, QList<DeviceBook> *clippings);

// _sync_kindle_thumbnails(): puts back the covers in system/thumbnails that
// Amazon replaced, from amazon-cover-bug. Returns how many.
int restoreThumbnails(DeviceFiles &files, QString *error);

} // namespace Kindle
