#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>

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

private:
    struct Page;
    PlacementBook();
    Page *page(int spineIndex);
    int spineIndexOf(const QString &contentId) const;

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
