#pragma once

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QProcess;

// What the jobs of Conversion, SendJob and MetadataDownload share: a folder
// of the cache holding the job, a calibre-debug that runs its Python and
// writes its log and result.json there, and lines of progress on its output.
namespace CalibreJob {

bool writeFile(const QString &path, const QByteArray &data, QString *error);
// The last lines of Calibre's log in the job's folder, which say what went
// wrong.
QString logTail(const QString &folder);

// Calibre in `interfaceLanguage`, with its temporary files in the job's
// folder.
QProcessEnvironment environment(const QString &folder, const QString &interfaceLanguage);
// The share of the work done that a line of the job's output reports, from
// 0 to 1; -1 for a line that reports none.
double progress(const QByteArray &line);

// Runs `program` to its end, telling `progress` how far it got. Its exit
// code, -1 when it crashed; nothing, with `error`, when it did not start.
std::optional<int> run(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment,
                       const std::function<void(double)> &progress, QString *error);

// Stops a job's process and those it started, its process group: asked
// first, killed after 3 s.
void stopProcessGroup(QProcess *process);
// The job folders in `base` a crash or a kill left behind, a day old:
// younger ones may be another Cousoir's on the same library.
void removeStaleFolders(const QString &base);

} // namespace CalibreJob
