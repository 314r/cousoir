#pragma once

#include <QString>

#include <memory>

#include "xml_tree.h"

// HTML as the old books have it, tags left open and closed out of turn,
// read into an XHTML tree, as a browser would mend it: a paragraph closed
// by the block after it, an item by the next, an end tag closing what is
// open inside it and one with nothing to close left out; the head's
// elements in the head, the rest in the body; scripts, comments and tags
// of other namespaces dropped; entities read.
namespace HtmlSoup {

// A document as XmlTree::parse() gives one: <html> with its <head> and
// <body> under a root that is no element of it.
std::unique_ptr<XmlTree::Node> parse(const QString &html);

} // namespace HtmlSoup
