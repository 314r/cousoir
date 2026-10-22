#include "calibre_job.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>

#include <signal.h>

namespace CalibreJob {

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString logTail(const QString &folder)
{
    QFile file(QDir(folder).filePath(QStringLiteral("log.txt")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QStringList lines = QString::fromUtf8(file.readAll()).trimmed().split(u'\n');
    return lines.mid(qMax(0, int(lines.size()) - 3)).join(u'\n').trimmed();
}

QProcessEnvironment environment(const QString &folder, const QString &interfaceLanguage)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString temporary = QDir(folder).filePath(QStringLiteral("tmp"));
    QDir().mkpath(temporary);
    environment.insert(QStringLiteral("TMPDIR"), temporary);
    if (!interfaceLanguage.isEmpty())
        environment.insert(QStringLiteral("CALIBRE_OVERRIDE_LANG"), interfaceLanguage);
    return environment;
}

double progress(const QByteArray &line)
{
    const QJsonObject object = QJsonDocument::fromJson(line.trimmed()).object();
    const QJsonValue value = object.value(QLatin1String("progress"));
    return value.isDouble() ? qBound(0.0, value.toDouble(), 1.0) : -1;
}

std::optional<int> run(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment,
                       const std::function<void(double)> &progress, QString *error)
{
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, arguments);
    if (!process.waitForStarted()) {
        if (error)
            *error = QObject::tr("Cannot run %1.").arg(program);
        return std::nullopt;
    }
    const auto readLines = [&] {
        while (process.canReadLine()) {
            const double fraction = CalibreJob::progress(process.readLine());
            if (fraction >= 0 && progress)
                progress(fraction);
        }
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        readLines();
    }
    readLines();
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

void stopProcessGroup(QProcess *process)
{
    const pid_t pid = pid_t(process->processId());
    if (pid > 0)
        ::kill(-pid, SIGTERM);
    if (!process->waitForFinished(3000) && pid > 0) {
        ::kill(-pid, SIGKILL);
        process->waitForFinished(1000);
    }
}

void removeStaleFolders(const QString &base)
{
    const QDateTime dayAgo = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &folder : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (folder.lastModified() < dayAgo)
            QDir(folder.filePath()).removeRecursively();
    }
}

} // namespace CalibreJob
