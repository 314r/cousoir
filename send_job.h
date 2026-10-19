#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

struct LibraryData;

// What sending books to a reader through Calibre takes, whatever the reader:
// a calibre-debug job in a folder of the cache running Calibre 9.16's "Send to
// device" as its GUI runs it, the GUI's own methods called on a stand-in for
// its window, with a copy of the library's database and of the books' files
// and covers. KoboSend and KindleSend give it the device and take its
// outcome.
namespace SendJob {

// The Python of the job before the device's part: the stand-in for Calibre's
// window and device manager, then the device's part's send(job), called by
// main(send) at its end, which writes result.json.
extern const char script[];

// Calibre's calibre-debug, empty when Calibre is not installed.
QString program();

// Writes into `folder`/library a copy of `data`'s metadata.db and, of each of
// `bookIds`, its cover and the file Calibre sends, the first of `formatMap`
// it has. A book without one is refused.
bool prepareLibrary(const LibraryData &data, const QList<int> &bookIds, const QStringList &formatMap,
                    const QString &folder, QString *error);
// Writes the job: `job` as job.json, with "books", and the script.
bool writeJob(const QString &folder, QJsonObject job, const QList<int> &bookIds, const QByteArray &deviceScript,
              QString *error);
bool copyInto(const QString &source, const QString &target, QString *error);

QStringList arguments(const QString &folder);
// Calibre in `interfaceLanguage`, with its temporary files in the job's
// folder.
QProcessEnvironment environment(const QString &folder, const QString &interfaceLanguage);
// The share of Calibre's part done that a line of the job's output reports,
// from 0 to 1; -1 for a line that reports none.
double progress(const QByteArray &line);

struct Sent
{
    int bookId = 0;
    QString lpath;
};

struct Result
{
    bool ok = false;
    QString error;
    QList<Sent> sent;
    // The titles of the books Calibre found nothing to send of.
    QStringList unsendable;
    // The rest of result.json, the device's.
    QJsonObject json;
};

// What the job in `folder` came to; `exitCode` is calibre-debug's.
Result result(const QString &folder, int exitCode);

// Runs the job to its end, telling `progress` how far Calibre got: for the
// command line and the tests.
Result run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress = {});

} // namespace SendJob
