#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "send_job.h"

class DeviceFiles;
struct LibraryData;

// Sending books to a Kindle over MTP as Calibre 9.16's "Send to device" does:
// the format its MTP driver prefers, the library's metadata and cover in a
// copy, the file in the documents folder named by the driver's template, its
// cover in system/thumbnails and amazon-cover-bug, its page numbers in an
// APNX file, and the book in metadata.calibre. Calibre's MTP driver runs in
// a SendJob on a stand-in for the Kindle: its files and folders as Cousoir
// listed them, and copies of the few Calibre reads. What the driver would do
// to them, folders made, files put and deleted, is noted; Cousoir then does
// it on the Kindle, through the MTP connection it keeps from the listing on,
// so that nothing else changes the Kindle meanwhile.
namespace KindleSend {

// What Cousoir knows of the Kindle that Calibre's driver asks.
struct Device
{
    QString friendlyName;
    QString modelName;
    QString serial;
    quint16 vendorId = 0;
    quint16 productId = 0;
};

// What a folder standing for a Kindle says of itself, in the tests.
Device standIn();

// Writes the job sending `bookIds` of `data` to the Kindle whose files
// `files` lists, into `folder`, which must exist and be empty.
bool prepare(const LibraryData &data, const QList<int> &bookIds, DeviceFiles &files, const Device &device,
             const QStringList &formatMap, const QString &folder, QString *error);

struct Operation
{
    enum Kind { MakeFolder, Put, Delete };
    Kind kind = Put;
    // On the Kindle, from its storage's root.
    QString path;
    // For a put, the file in the job's folder.
    QString file;
};

struct Outcome
{
    bool ok = false;
    QString error;
    QList<SendJob::Sent> sent;
    QStringList unsendable;
    QList<Operation> operations;
};

// The send without Calibre: writes into `folder` what Calibre's job would
// leave there for the same books: each copy with the library's metadata
// written in, as MetadataUpdater writes it into a MOBI or AZW3, at the MTP
// driver's send template, its cover thumbnail, kept against Amazon's bug,
// its APNX page numbers by the fast method, then metadata.calibre and
// driveinfo.calibre; the operations putting them on the Kindle, which
// outcome() reads and apply() does. Calibre's settings in
// `configDirectory` apply: the MTP driver's for this Kindle, the Kindle
// driver's APNX, MOBI output's.
bool prepareNatively(const LibraryData &data, const QList<int> &bookIds, DeviceFiles &files, const Device &device,
                     const QStringList &formatMap, const QString &configDirectory, const QString &interfaceLanguage,
                     const QString &folder, QString *error);

// What the job in `folder` came to; `exitCode` is calibre-debug's.
Outcome outcome(const QString &folder, int exitCode);
// The same from the job's result as SendJob read it.
Outcome outcome(const SendJob::Result &result);

// Does the job's operations on the Kindle through `files`, a file put twice
// only the last time. `progress` hears of the bytes copied.
bool apply(const QString &folder, const Outcome &outcome, DeviceFiles &files, QString *error,
           const std::function<void(double)> &progress = {});

// Runs the job to its end and returns its outcome: for the command line and
// the tests.
Outcome run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress = {});

} // namespace KindleSend
