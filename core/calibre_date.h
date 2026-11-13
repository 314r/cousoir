#pragma once

#include <QDateTime>
#include <QString>

// A date and time as Python's datetime holds it: QDateTime stops at the
// millisecond, Calibre keeps and writes the microseconds of the dates it
// reads.
struct CalibreDate
{
    // UTC, to the second.
    QDateTime dateTime;
    int microsecond = 0;

    // The date as Calibre's isoformat() writes it, with "T" or " " between
    // date and time: "2025-11-11 10:50:50.009108+00:00".
    QString isoFormat(QChar separator = u'T') const
    {
        const QDateTime utc = dateTime.toUTC();
        QString text = QStringLiteral("%1-%2-%3%4%5")
                           .arg(utc.date().year(), 4, 10, QLatin1Char('0'))
                           .arg(utc.date().month(), 2, 10, QLatin1Char('0'))
                           .arg(utc.date().day(), 2, 10, QLatin1Char('0'))
                           .arg(separator)
                           .arg(utc.time().toString(QStringLiteral("HH:mm:ss")));
        if (microsecond > 0)
            text += QStringLiteral(".%1").arg(microsecond, 6, 10, QLatin1Char('0'));
        return text + QStringLiteral("+00:00");
    }

    static CalibreDate fromDateTime(const QDateTime &value)
    {
        const QDateTime utc = value.toUTC();
        CalibreDate date;
        date.microsecond = utc.time().msec() * 1000;
        date.dateTime = utc.addMSecs(-utc.time().msec());
        return date;
    }

    friend bool operator==(const CalibreDate &a, const CalibreDate &b)
    {
        return a.dateTime == b.dateTime && a.microsecond == b.microsecond;
    }
    friend bool operator<(const CalibreDate &a, const CalibreDate &b)
    {
        return a.dateTime < b.dateTime || (a.dateTime == b.dateTime && a.microsecond < b.microsecond);
    }
};
