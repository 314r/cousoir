// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from kbextract2, https://github.com/314r/kbextract2, Copyright (c)
// 2026 314r, MIT license: internal/kobo/format.go, formatAnnotations(), and
// internal/kobo/text.go.

#include "highlight_formats.h"

#include <QHash>

#include <algorithm>

namespace HighlightFormats {

namespace {

QString highlightBlock(const QString &text)
{
    QStringList lines = reflowedHighlight(text).split(u'\n');
    for (QString &line : lines)
        line = line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line;
    return lines.join(u'\n');
}

} // namespace

Format formatNamed(const QString &name)
{
    if (name == QLatin1String("obsidian"))
        return Format::Obsidian;
    if (name == QLatin1String("plain"))
        return Format::Plain;
    return Format::Markdown;
}

QString normalizedText(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(u'\r', u'\n');
    return normalized.trimmed();
}

QString singleLineText(const QString &text)
{
    return normalizedText(text).simplified();
}

QString reflowedHighlight(const QString &text)
{
    QStringList paragraphs;
    QStringList lines;
    const auto flush = [&] {
        if (!lines.isEmpty()) {
            paragraphs.append(lines.join(u' '));
            lines.clear();
        }
    };
    for (const QString &line : normalizedText(text).split(u'\n')) {
        const QString fragment = line.trimmed();
        if (fragment.isEmpty())
            flush();
        else
            lines.append(fragment);
    }
    flush();
    return paragraphs.join(QStringLiteral("\n\n"));
}

QList<Chapter> chapters(const QList<Entry> &entries)
{
    struct Found
    {
        Chapter chapter;
        std::optional<qint64> order;
        int firstSeen = 0;
    };
    QList<Found> found;
    QHash<QString, int> indexes;
    int anonymous = 0;
    for (const Entry &entry : entries) {
        if (entry.highlight.isEmpty() && entry.note.isEmpty())
            continue;
        const QString key = !entry.chapterKey.isEmpty() ? QStringLiteral("chapter:") + entry.chapterKey
            : !entry.id.isEmpty()                       ? QStringLiteral("bookmark:") + entry.id
                                                        : QStringLiteral("anonymous:%1").arg(anonymous++);
        auto index = indexes.constFind(key);
        if (index == indexes.cend()) {
            index = indexes.insert(key, int(found.size()));
            found.append({ { entry.chapterTitle, {} }, entry.chapterOrder, int(found.size()) });
        } else {
            Found &chapter = found[*index];
            if (chapter.chapter.title.isEmpty())
                chapter.chapter.title = entry.chapterTitle;
            if (!chapter.order)
                chapter.order = entry.chapterOrder;
        }
        found[*index].chapter.entries.append(entry);
    }
    std::stable_sort(found.begin(), found.end(), [](const Found &left, const Found &right) {
        if (left.order && right.order && *left.order != *right.order)
            return *left.order < *right.order;
        if (left.order.has_value() != right.order.has_value())
            return left.order.has_value();
        return left.firstSeen < right.firstSeen;
    });
    QList<Chapter> result;
    result.reserve(found.size());
    for (Found &chapter : found)
        result.append(std::move(chapter.chapter));
    return result;
}

QString render(const QList<Entry> &entries, Format format)
{
    QStringList sections;
    for (const Chapter &chapter : chapters(entries)) {
        QStringList blocks;
        for (const Entry &entry : chapter.entries) {
            QStringList parts;
            if (!entry.highlight.isEmpty()) {
                if (format == Format::Plain)
                    parts.append(reflowedHighlight(entry.highlight));
                else if (format == Format::Obsidian)
                    parts.append(QStringLiteral("> [!quote]\n") + highlightBlock(entry.highlight));
                else
                    parts.append(highlightBlock(entry.highlight));
            }
            if (!entry.note.isEmpty())
                parts.append(normalizedText(entry.note));
            blocks.append(parts.join(QStringLiteral("\n\n")));
        }
        const QString title = chapter.title.isEmpty() ? QStringLiteral("Untitled chapter") : chapter.title;
        const QString heading = format == Format::Plain ? title : QStringLiteral("## ") + title;
        sections.append(heading + QStringLiteral("\n\n") + blocks.join(QStringLiteral("\n\n\n")));
    }
    return sections.join(QStringLiteral("\n\n\n"));
}

QString render(const QList<Book> &books, Format format)
{
    QStringList parts;
    for (const Book &book : books) {
        const QString body = render(book.entries, format);
        if (body.isEmpty())
            continue;
        const QString heading = format == Format::Plain ? book.title : QStringLiteral("# ") + book.title;
        parts.append(heading + QStringLiteral("\n\n") + body);
    }
    return parts.join(QStringLiteral("\n\n\n"));
}

} // namespace HighlightFormats
