#pragma once

#include <QList>
#include <QString>

#include <optional>

// Highlights written out as kbextract2 writes a Kobo book's, to be pasted
// into notes: by chapter, each highlight a quote and its note after it, as
// Markdown, as Obsidian's Markdown, whose quotes are callouts, or as plain
// text.
namespace HighlightFormats {

enum class Format { Markdown, Obsidian, Plain };

struct Entry
{
    QString id;
    QString highlight;
    QString note;
    // The entries of one key go together under its title, the chapters in
    // their order, those without one after, as they come.
    QString chapterKey;
    QString chapterTitle;
    std::optional<qint64> chapterOrder;
};

struct Book
{
    QString title;
    QList<Entry> entries;
};

// A book's chapters as render() writes them: in their order, those without
// one after, as they come; each with its entries in the order given.
struct Chapter
{
    QString title;
    QList<Entry> entries;
};

// "markdown", "obsidian" or "plain"; Markdown for any other.
Format formatNamed(const QString &name);

// The entries with a highlight or a note, by chapter; an untitled chapter's
// title empty.
QList<Chapter> chapters(const QList<Entry> &entries);

// A book's entries: no title, a heading by chapter.
QString render(const QList<Entry> &entries, Format format);
// Several books, each under its title.
QString render(const QList<Book> &books, Format format);

// CR and CRLF as LF, the ends trimmed.
QString normalizedText(const QString &text);
// Every run of spaces and line ends as one space.
QString singleLineText(const QString &text);
// A highlight's lines joined by a space, a blank line between paragraphs.
QString reflowedHighlight(const QString &text);

} // namespace HighlightFormats
