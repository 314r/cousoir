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

struct Chapter
{
    QString title;
    QStringList blocks;
    std::optional<qint64> order;
    int firstSeen = 0;
};

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

QString render(const QList<Entry> &entries, Format format)
{
    QList<Chapter> chapters;
    QHash<QString, int> indexes;
    int anonymous = 0;
    for (const Entry &entry : entries) {
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
        if (parts.isEmpty())
            continue;
        const QString key = !entry.chapterKey.isEmpty() ? QStringLiteral("chapter:") + entry.chapterKey
            : !entry.id.isEmpty()                       ? QStringLiteral("bookmark:") + entry.id
                                                        : QStringLiteral("anonymous:%1").arg(anonymous++);
        auto found = indexes.constFind(key);
        if (found == indexes.cend()) {
            found = indexes.insert(key, int(chapters.size()));
            chapters.append({ entry.chapterTitle, {}, entry.chapterOrder, int(chapters.size()) });
        } else {
            Chapter &chapter = chapters[*found];
            if (chapter.title.isEmpty())
                chapter.title = entry.chapterTitle;
            if (!chapter.order)
                chapter.order = entry.chapterOrder;
        }
        chapters[*found].blocks.append(parts.join(QStringLiteral("\n\n")));
    }
    std::stable_sort(chapters.begin(), chapters.end(), [](const Chapter &left, const Chapter &right) {
        if (left.order && right.order && *left.order != *right.order)
            return *left.order < *right.order;
        if (left.order.has_value() != right.order.has_value())
            return left.order.has_value();
        return left.firstSeen < right.firstSeen;
    });
    QStringList sections;
    for (const Chapter &chapter : std::as_const(chapters)) {
        const QString title = chapter.title.isEmpty() ? QStringLiteral("Untitled chapter") : chapter.title;
        const QString heading = format == Format::Plain ? title : QStringLiteral("## ") + title;
        sections.append(heading + QStringLiteral("\n\n") + chapter.blocks.join(QStringLiteral("\n\n\n")));
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
