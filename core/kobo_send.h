#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "kobo.h"
#include "send_job.h"

struct Book;
struct LibraryData;

// Sending books to a Kobo as Calibre 9.16's "Send to device" does: the
// format KoboTouch prefers, the library's metadata and cover put into a copy
// of the file, an EPUB made a KEPUB on the way unless the driver's settings
// say otherwise, the file placed by the driver's save template, and its
// metadata added to metadata.calibre. Calibre does it, through calibre-debug,
// on a copy of what it reads of the library and of the Kobo: the job's
// "device" folder stands for the Kobo, and takes what Calibre would write
// there. Cousoir then puts the new files on the Kobo itself, the books
// first and metadata.calibre last, and leaves the Kobo's own database alone.
//
// A job is SendJob's, with the copies of the Kobo's files under "device".
namespace KoboSend {

// Writes the job sending `bookIds` of `data` to `device` into `folder`,
// which must exist and be empty. `formatMap` is KoboTouch's order of
// formats; a book without one of them is refused.
bool prepare(const LibraryData &data, const QList<int> &bookIds, const Kobo::Device &device,
             const QStringList &formatMap, const QString &folder, QString *error);

struct Outcome
{
    bool ok = false;
    QString error;
    QList<SendJob::Sent> sent;
    // The titles of the books Calibre found nothing to send of.
    QStringList unsendable;
    // What Calibre wrote for the Kobo, relative to the job's device folder:
    // book files first, then driveinfo.calibre and metadata.calibre.
    QStringList files;
    // Calibre deletes the Kobo's cover cache of each book sent unless it
    // sends covers itself.
    bool clearCovers = true;
};

// What Cousoir follows of Calibre's KoboTouch settings when it sends without
// Calibre: device_drivers_KOBOTOUCH.py.json, else Calibre's defaults.
struct DriverSettings
{
    QString saveTemplate = QStringLiteral("{author_sort}/{title} - {authors}");
    bool kepubify = true;
    bool useSubdirs = true;
};
DriverSettings driverSettings(const QString &configDirectory);

// The send without Calibre: writes into `folder` what Calibre's job would
// leave there, the books as KoboTouch puts them, an EPUB as a KEPUB with the
// library's metadata and cover, at the driver's save template, and
// metadata.calibre and driveinfo.calibre with them, then its result, which
// outcome() reads and apply() puts on the Kobo.
bool prepareNatively(const LibraryData &data, const QList<int> &bookIds, const Kobo::Device &device,
                     const QStringList &formatMap, const QString &configDirectory, const QString &interfaceLanguage,
                     const QString &folder, QString *error);

// KoboTouch's create_upload_path(): where a book goes on a Kobo mounted at
// `root`, relative to it, `extension` its file's, ".epub" or ".pdf".
QString uploadPath(const LibraryData &data, const Book &book, const QString &root, const QString &extension, bool kepub,
                   const DriverSettings &settings, const QString &interfaceLanguage);

// What the job in `folder` came to; `exitCode` is calibre-debug's.
Outcome outcome(const QString &folder, int exitCode);
// The same from the job's result as SendJob read it.
Outcome outcome(const SendJob::Result &result);

// Puts what the job wrote on `device`, unless the Kobo's metadata.calibre
// changed since prepare(). `progress` hears of the bytes copied.
bool apply(const QString &folder, const Outcome &outcome, const Kobo::Device &device, QString *error,
           const std::function<void(double)> &progress = {});

// Runs the job to its end and returns its outcome: for the command line and
// the tests.
Outcome run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress = {});

} // namespace KoboSend
