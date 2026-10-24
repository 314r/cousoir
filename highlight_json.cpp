// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/annotations.py: annot_db_data()
// - src/pyj/read_book/annotations.pyj: a highlight's fields, remove_highlight()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "highlight_json.h"

#include <QJsonArray>

#include "epub_book.h"

namespace HighlightJson {

QString isoTimestamp(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QString searchableText(const QJsonObject &highlight)
{
    QString text = highlight.value(QLatin1String("highlighted_text")).toString();
    const QString notes = highlight.value(QLatin1String("notes")).toString();
    if (!notes.isEmpty())
        text += QStringLiteral("\n\x1f\n") + notes;
    return text.normalized(QString::NormalizationForm_KC);
}

QJsonObject highlight(const QString &uuid, const QString &timestamp, const QString &text, const QString &startCfi,
                      const QString &endCfi, const QString &which, const QString &spineName, int spineIndex,
                      const QStringList &tocFamily, const QString &notes)
{
    QJsonObject highlight{
        { QStringLiteral("type"), QStringLiteral("highlight") },
        { QStringLiteral("timestamp"), timestamp },
        { QStringLiteral("uuid"), uuid },
        { QStringLiteral("highlighted_text"), text },
        { QStringLiteral("start_cfi"), startCfi },
        { QStringLiteral("end_cfi"), endCfi },
        { QStringLiteral("style"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("builtin") },
                                                { QStringLiteral("kind"), QStringLiteral("color") },
                                                { QStringLiteral("which"), which } } },
        { QStringLiteral("spine_name"), spineName },
        { QStringLiteral("spine_index"), spineIndex },
    };
    if (!tocFamily.isEmpty())
        highlight.insert(QStringLiteral("toc_family_titles"), QJsonArray::fromStringList(tocFamily));
    if (!notes.isEmpty())
        highlight.insert(QStringLiteral("notes"), notes);
    return highlight;
}

QJsonObject removed(const QString &uuid, const QString &timestamp)
{
    return {
        { QStringLiteral("type"), QStringLiteral("highlight") },
        { QStringLiteral("timestamp"), timestamp },
        { QStringLiteral("uuid"), uuid },
        { QStringLiteral("removed"), true },
    };
}

QStringList tocFamily(const EpubBook &book, int spineIndex, const std::function<bool(const QString &fragment)> &before)
{
    QStringList family;
    for (const EpubBook::TocEntry &entry : book.toc()) {
        const int index = book.spineIndex(entry.name);
        if (index < 0 || index > spineIndex)
            continue;
        if (index == spineIndex && !entry.fragment.isEmpty() && before && !before(entry.fragment))
            continue;
        while (family.size() > entry.depth)
            family.removeLast();
        family.append(entry.title);
    }
    return family;
}

} // namespace HighlightJson
