#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include "xml_tree.h"

// The CFIs Calibre's viewer and Cousoir's reader write for a place in a
// page, "/2/4/6[note]/1:22", worked out from the page's XML alone: what
// Calibre's cfi.pyj gets from the browser's document. Only elements and the
// characters of the text count, as there: a page parsed by XmlTree gives the
// browser's CFIs.
namespace EpubCfi {

// A place in a page: a node and, for a text, an offset in UTF-16 units.
struct Point
{
    const XmlTree::Node *node = nullptr;
    qsizetype offset = 0;
};

class Document
{
public:
    // `root` as XmlTree::parse() gives it; it must outlive the Document.
    explicit Document(const XmlTree::Node &root);

    // encode(): the CFI of `offset` in the text `node`; of the element
    // `node`, or of its child element at `offset` when one is given.
    QString encode(const XmlTree::Node &node, qsizetype offset = -1) const;
    // decode(): the place a CFI names; a null node when it names none.
    Point decode(const QString &cfi) const;
    // The text between two places in texts, as a Range's toString() gives
    // it.
    QString textBetween(Point start, Point end) const;
    // The first element of the id, as getElementById() finds it; null for
    // none.
    const XmlTree::Node *element(const QString &id) const;

private:
    const XmlTree::Node &m_root;
    // How many elements have each id, for id_is_unique().
    QHash<QString, int> m_ids;
    QHash<QString, const XmlTree::Node *> m_firstById;
};

// A page's text as a reader reads it: the characters of the texts of its
// body, in order, each run of white space as one space and the characters
// that show nothing (soft hyphens, zero widths) left out; and where in the
// page each character starts and ends, for the CFIs of a part of it.
struct PageText
{
    QString text;
    QList<Point> starts;
    QList<Point> ends;
    // The page's nodes, by their place in document order.
    QHash<const XmlTree::Node *, int> order;
    // With blockBreaks, where in the text blocks end: the index of each
    // space between two blocks.
    QList<qsizetype> breaks;

    // The text of the body of the page `root`, or of all of it without one;
    // with `blockBreaks`, a space between blocks, paragraphs and headings,
    // the page has none between, as a reader sees one.
    static PageText of(const XmlTree::Node &root, bool blockBreaks = false);
    // The index of the first character at or after a place in a text; -1
    // for none.
    qsizetype indexOf(const Point &point) const;
};

// Whether a character shows nothing, which PageText leaves out.
bool invisible(QChar c);

// escape_for_cfi() and unescape_from_cfi().
QString escape(const QString &raw);
QString unescape(const QString &raw);

} // namespace EpubCfi
