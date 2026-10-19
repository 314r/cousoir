#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct LibraryData;

// A Kobo connected over USB, as Calibre 9.16's KoboTouch driver sees it: the
// folder its storage is mounted on, with Kobo's own .kobo folder in it.
// Calibre keeps two files at its root: metadata.calibre, the books it sent
// with their library metadata, and driveinfo.calibre, the device's name and
// the library it last met. Reliure reads and writes both as Calibre does.
// The Kobo's database, .kobo/KoboReader.sqlite, which Calibre edits too, it
// only reads: the Kobo finds the books sent or removed by itself.
namespace Kobo {

struct Device
{
    // The folder its storage is mounted on, without a trailing slash.
    QString root;
    // Its block device, like /dev/sdb.
    QString node;
    // driveinfo.calibre's name once Calibre or Reliure wrote one, else the
    // name Calibre gives its model.
    QString name;
    // .kobo/version: serial number, firmware, and the model id that tells
    // apart models sharing a USB product id.
    QString serial;
    QString firmware;
    QString modelId;
    // The USB product id, 0 when unknown.
    int productId = 0;
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
};

// Calibre's is_main_drive(): a .kobo folder at the root.
bool isKobo(const QString &root);
// The Kobo mounted on `root` from block device `node`; its USB ids come from
// `sysRoot`, Linux's /sys.
Device describe(const QString &root, const QString &node, const QString &sysRoot = QStringLiteral("/sys"));
// The USB product id of the device behind block device `node`, 0 when there
// is none to find.
int usbProductId(const QString &node, const QString &sysRoot = QStringLiteral("/sys"));
// set_device_name(): the model's name for its USB product id and model id,
// "Kobo eReader" when unknown.
QString modelName(int productId, const QString &modelId);
// The firmware version as numbers, (0, 0, 0) when unknown.
QList<int> firmwareVersion(const Device &device);

struct DeviceBook
{
    // Calibre's lpath: the file's path from the root, / separated.
    QString lpath;
    QString title;
    QStringList authors;
    QString authorSort;
    // From metadata.calibre: the library book it was sent from.
    QString uuid;
    int applicationId = 0;
    qint64 size = 0;
    // As the library names formats: KEPUB, EPUB, PDF…
    QString format;
    // In metadata.calibre: Calibre or Reliure sent it.
    bool sent = false;
    // In the Kobo's database: the Kobo has taken it in.
    bool imported = false;
    // Bought from Kobo and kept in .kobo/kepub; not a file one sends or
    // removes without editing the Kobo's database.
    bool purchased = false;
    // The Kobo's id for its cover images, when its database has one.
    QString imageId;
    // Set by match(): the library book it is, 0 when none.
    int bookId = 0;
};

// The books on the Kobo as KoboTouch's books() lists them: the Kobo's
// database's, sideloaded or bought, with the metadata Calibre keeps for those
// it sent, then those sent since the Kobo last looked, whose files are there
// but not yet in its database.
QList<DeviceBook> books(const Device &device, QString *error);

// set_books_in_library() of Calibre's GUI: each book is the library's book
// with its uuid, else the one with its title and its authors, author sort or
// one of its authors, punctuation and case aside.
void match(QList<DeviceBook> *books, const LibraryData &data);

// The format Calibre sends of `formats`, the book's: the first of
// `formatMap`, the driver's ordered formats, the book has. Empty for none.
QString sendFormat(const QStringList &formats, const QStringList &formatMap);
// KoboTouch's formats in the order Calibre prefers them, unless changed in
// its device settings: device_drivers_KOBOTOUCH.py.json in `configDirectory`.
QStringList formatMap(const QString &configDirectory);

// The files of the Kobo's cover cache for the book at `lpath`, whose image
// id the Kobo's database gives or else Calibre's imageid_from_contentid():
// Calibre deletes them when it sends or removes the book, so that the Kobo
// makes them again from the new file.
QStringList coverCache(const Device &device, const QString &lpath, const QString &imageId = {});
// Deletes those files and the folders they leave empty.
void clearCoverCache(const Device &device, const QString &lpath, const QString &imageId = {});

// Removes books from the Kobo as KOBO.delete_books() and
// remove_books_from_metadata() do, but for the Kobo's database: the files,
// their cover cache and the folders they leave empty, then their entries in
// metadata.calibre, whose others stay as written. Bought books stay.
bool removeBooks(const Device &device, const QList<DeviceBook> &books, QString *error);

// metadata.calibre's books, as JsonCodec writes them; none when it is
// missing.
QJsonArray readMetadata(const QString &root, QString *error = nullptr);
QJsonObject readDriveInfo(const QString &root);

// Writes `data` to `path` on the device through a hidden temporary file
// renamed over it, synced first: a file is whole or not there.
bool writeFile(const QString &path, const QByteArray &data, QString *error);
// Copies `source` to `path` on the device the same way.
bool copyFile(const QString &source, const QString &path, QString *error);

} // namespace Kobo
