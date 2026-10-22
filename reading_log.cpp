#include "reading_log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>

namespace {

QJsonObject toJson(const ReadingLog::Entry &entry)
{
    QJsonArray notes;
    for (const ReaderNote &note : entry.reading.notes) {
        notes.append(QJsonObject{
            { QStringLiteral("type"), note.type },
            { QStringLiteral("text"), note.text },
            { QStringLiteral("annotation"), note.annotation },
            { QStringLiteral("chapter"), note.chapter },
            { QStringLiteral("chapterTitle"), note.chapterTitle },
            { QStringLiteral("chapterProgress"), note.chapterProgress },
            { QStringLiteral("location"), note.location },
            { QStringLiteral("created"), note.created },
        });
    }
    return {
        { QStringLiteral("device"), entry.device },
        { QStringLiteral("deviceName"), entry.deviceName },
        { QStringLiteral("lpath"), entry.lpath },
        { QStringLiteral("format"), entry.format },
        { QStringLiteral("seen"), entry.seen.toUTC().toString(Qt::ISODate) },
        { QStringLiteral("status"), entry.reading.status },
        { QStringLiteral("percent"), entry.reading.percent ? QJsonValue(*entry.reading.percent) : QJsonValue() },
        { QStringLiteral("lastRead"), entry.reading.lastRead },
        { QStringLiteral("secondsSpent"), entry.reading.secondsSpent },
        { QStringLiteral("notes"), notes },
    };
}

ReadingLog::Entry fromJson(const QJsonObject &object)
{
    ReadingLog::Entry entry;
    entry.device = object.value(QLatin1String("device")).toString();
    entry.deviceName = object.value(QLatin1String("deviceName")).toString();
    entry.lpath = object.value(QLatin1String("lpath")).toString();
    entry.format = object.value(QLatin1String("format")).toString();
    entry.seen = QDateTime::fromString(object.value(QLatin1String("seen")).toString(), Qt::ISODate);
    entry.reading.status = object.value(QLatin1String("status")).toInt();
    if (object.value(QLatin1String("percent")).isDouble())
        entry.reading.percent = object.value(QLatin1String("percent")).toInt();
    entry.reading.lastRead = object.value(QLatin1String("lastRead")).toString();
    entry.reading.secondsSpent = object.value(QLatin1String("secondsSpent")).toInteger();
    for (const QJsonValue &value : object.value(QLatin1String("notes")).toArray()) {
        const QJsonObject note = value.toObject();
        entry.reading.notes.append(ReaderNote{
            note.value(QLatin1String("type")).toString(),
            note.value(QLatin1String("text")).toString(),
            note.value(QLatin1String("annotation")).toString(),
            note.value(QLatin1String("chapter")).toInt(),
            note.value(QLatin1String("chapterTitle")).toString(),
            note.value(QLatin1String("chapterProgress")).toDouble(),
            note.value(QLatin1String("location")).toInt(),
            note.value(QLatin1String("created")).toString(),
        });
    }
    return entry;
}

} // namespace

namespace ReadingLog {

Log read(const QString &file)
{
    Log log;
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return log;
    const QJsonObject books = QJsonDocument::fromJson(in.readAll()).object();
    for (auto it = books.constBegin(); it != books.constEnd(); ++it) {
        QList<Entry> entries;
        for (const QJsonValue &entry : it.value().toArray())
            entries.append(fromJson(entry.toObject()));
        if (!entries.isEmpty())
            log.insert(it.key(), entries);
    }
    return log;
}

bool write(const QString &file, const Log &log, QString *error)
{
    QJsonObject books;
    for (auto it = log.constBegin(); it != log.constEnd(); ++it) {
        QJsonArray entries;
        for (const Entry &entry : it.value())
            entries.append(toJson(entry));
        books.insert(it.key(), entries);
    }
    QSaveFile out(file);
    if (!QDir().mkpath(QFileInfo(file).absolutePath()) || !out.open(QIODevice::WriteOnly)
        || out.write(QJsonDocument(books).toJson(QJsonDocument::Compact)) < 0 || !out.commit()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(file, out.errorString());
        return false;
    }
    return true;
}

void replaceDevice(Log *log, const QString &device, const QHash<QString, QList<Entry>> &entries)
{
    for (auto it = log->begin(); it != log->end();) {
        it->removeIf([&device](const Entry &entry) { return entry.device == device; });
        it = it->isEmpty() ? log->erase(it) : std::next(it);
    }
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        (*log)[it.key()].append(it.value());
}

} // namespace ReadingLog
