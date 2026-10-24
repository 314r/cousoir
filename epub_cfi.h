#pragma once

#include <QHash>
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

private:
    const XmlTree::Node &m_root;
    // How many elements have each id, for id_is_unique().
    QHash<QString, int> m_ids;
    QHash<QString, const XmlTree::Node *> m_firstById;
};

// escape_for_cfi() and unescape_from_cfi().
QString escape(const QString &raw);
QString unescape(const QString &raw);

} // namespace EpubCfi
