#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>
#include <optional>

#include "kobo_highlights.h"

class EpubBook;
struct Book;

// Where a Kobo's highlight is in the library's copy of its book, by the
// CFIs Calibre's viewer and Cousoir's reader keep a highlight by: the
// Kobo's own place in the KEPUB it read, traced back to the library's EPUB
// through Kobo's spans as kepubify makes them, else its text found there.
namespace KoboPlacement {

enum class Method {
    None,
    // Kobo's spans, as kepubify makes them of the book, the Kobo's text
    // found between them.
    Spans,
    // The Kobo's text, found in the file it is in.
    Text,
    // The Kobo's text, found in another file of the book.
    Book,
};

struct Placement
{
    Method method = Method::None;
    int spineIndex = -1;
    QString spineName;
    QString startCfi;
    QString endCfi;
    // The table of contents entries it is under, as the viewer notes them.
    QStringList tocFamily;
    // Why it is nowhere.
    QString reason;
};

// "spans", "text", "book" or "none".
QString methodName(Method method);

// Where the Kobo stopped, in the library's copy of the book: a file of it
// and the CFI in it.
struct Spot
{
    Method method = Method::None;
    int spineIndex = -1;
    QString spineName;
    // In the file, "/2/4/6/1:0", without the file's step.
    QString cfi;
};

// A place in the book as its text tells it, to be found in another copy of
// the book: its file, where it starts in the file's text, white space
// folded, how long that text is, and the text there.
struct TextSpot
{
    QString spineName;
    qsizetype at = -1;
    qsizetype length = 0;
    QString text;
};

// A place in the KEPUB a Kobo reads, as the Kobo keeps one: Kobo's spans it
// starts and ends in and offsets in them, how far into its file it starts.
struct SpanRange
{
    QString spineName;
    QString startSpan;
    qint64 startOffset = 0;
    QString endSpan;
    qint64 endOffset = 0;
    double progress = 0;
};

// The file of the book a Kobo's id names: "OEBPS/Text/c1.xhtml" of a
// KEPUB's "…/book.kepub.epub!!OEBPS/Text/c1.xhtml",
// "…/book.kepub.epub!!OEBPS/Text/c1.xhtml#d14e4446" or
// "…/book.kepub.epub!OEBPS!Text/c1.xhtml", of an EPUB's
// "…/book.epub#(3)OEBPS/Text/c1.xhtml".
QString fileOf(const QString &contentId);

// The place of Kobo's span `id`, "kobo.3.1", as the Kobo writes it in a
// highlight, "span#kobo\.3\.1".
QString spanPath(const QString &id);

// The format of a library book its highlights are kept with, the one its
// reader opens: EPUB, else KEPUB; empty when it has neither.
QString targetFormat(const Book &book);

// A library book's file, read once for all the highlights placed in it.
class PlacementBook
{
public:
    // The EPUB or KEPUB at `path`; `format` says which.
    static std::unique_ptr<PlacementBook> open(const QString &path, const QString &format, QString *error);
    ~PlacementBook();

    QString format() const { return m_format; }
    Placement place(const KoboHighlights::Highlight &highlight);
    // Where the Kobo's place `bookmark`, "OEBPS/c2.xhtml#kobo.12.1", is in
    // the book: its span, else `chapterProgress` into its file.
    Spot placePoint(const QString &bookmark, double chapterProgress);

    // What the book has at a place, `startCfi` to `endCfi` of the file
    // `spineIndex`, or from `startCfi` on when `endCfi` is empty: then
    // nothing in a file with no text.
    std::optional<TextSpot> textAt(int spineIndex, const QString &startCfi, const QString &endCfi);
    // In a KEPUB: where the text of `spot` is, nearest where it is in the
    // other copy, by Kobo's spans, the file's start for no text; none when
    // the text is not there.
    std::optional<SpanRange> spansAt(const TextSpot &spot);
    // The start of the file `spineName` of a KEPUB, its first span.
    SpanRange startOf(const QString &spineName);

private:
    struct Page;
    PlacementBook();
    Page *page(int spineIndex);
    int spineIndexOf(const QString &contentId) const;
    int spineIndexOfFile(const QString &name) const;

    std::unique_ptr<EpubBook> m_book;
    QString m_format;
    std::map<int, std::unique_ptr<Page>> m_pages;
};

// When the Kobo last changed the highlight: its DateModified, else its
// DateCreated, read as UTC when they name no zone; invalid when neither
// reads.
QDateTime changedAt(const KoboHighlights::Highlight &highlight);

// The highlight placed, as Calibre's viewer keeps one: the Kobo's text,
// note and colour, its id the Kobo's.
QJsonObject annotation(const KoboHighlights::Highlight &highlight, const Placement &placement);

// The text as compared: spaces as one, soft hyphens and zero width
// characters out, trimmed.
QString folded(const QString &text);

} // namespace KoboPlacement
