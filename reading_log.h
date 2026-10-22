#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include "device_books.h"

// What the readers last told of the reading of a library's books, kept by
// Cousoir outside the library, so that a book's page still shows it once
// the reader is unplugged. Neither the library nor the reader is written.
namespace ReadingLog {

struct Entry
{
    // The reader, by its serial number or else its name, and its name.
    QString device;
    QString deviceName;
    // The book's file on it, and its format.
    QString lpath;
    QString format;
    // When the reader last told it.
    QDateTime seen;
    ReadingState reading;
};

// By the library book's uuid.
using Log = QHash<QString, QList<Entry>>;

// The log in `file`; empty when there is none or it cannot be read.
Log read(const QString &file);
bool write(const QString &file, const Log &log, QString *error = nullptr);
// What `device` tells now, by book uuid, in place of what it told before.
void replaceDevice(Log *log, const QString &device, const QHash<QString, QList<Entry>> &entries);
// Whether `device` told the same before, on the same day.
bool sameEntries(const Log &log, const QString &device, const QHash<QString, QList<Entry>> &entries, const QDate &today);

} // namespace ReadingLog
