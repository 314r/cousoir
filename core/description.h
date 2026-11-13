#pragma once

#include <QString>

// A book's description, which Calibre keeps as HTML, edited as text: blank
// lines between paragraphs, a line break where a line ends, and the marks
// of Markdown a description needs, **bold**, *italics*, [links](url),
// "- " and "1. " for lists, "#" for headings. What the text cannot hold,
// tables or images, a save leaves out.
namespace Description {

// The HTML the book's page shows: without the fonts, sizes and colours
// editors leave behind, which the theme gives instead; a description of
// plain text, as Calibre stores some, in paragraphs.
QString cleaned(const QString &html);

// The text the editor shows for a description.
QString toText(const QString &html);

// The HTML a text saves as, in paragraphs; empty for a blank text.
QString toHtml(const QString &text);

// Whether the description holds something its text leaves out: a table, an
// image, a rule, a quotation, preformatted text.
bool losesFormatting(const QString &html);

// Whether a note holds something its text leaves out: what a description
// would lose, and what Calibre's notes editor also makes, underlines,
// strikes, colours and other styles, alignment, super- and subscripts.
bool noteLosesFormatting(const QString &html);

} // namespace Description
