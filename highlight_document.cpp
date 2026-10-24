// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from kbextract2, https://github.com/314r/kbextract2, Copyright (c)
// 2026 314r, MIT license: internal/ui/document.go and readerview.go.

#include "highlight_document.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace {

// document.go's, in its pixels.
constexpr qreal sideMargin = 36;
constexpr qreal quoteBarWidth = 3;
constexpr qreal quoteIndent = 19;
constexpr qreal notePadding = 16;
constexpr qreal noteVPadding = 12;
constexpr qreal entryGap = 24;
constexpr qreal pairGap = 10;
constexpr qreal paragraphGap = 10;
constexpr qreal chapterGap = 44;
constexpr qreal headingGap = 14;
constexpr qreal ruleGap = 22;
constexpr int measureChars = 70;
// readerview.go's.
constexpr qreal sourceSideMargin = 32;

// A note's lines in one paragraph, so that its card holds them all.
QString softLines(QString text)
{
    text.replace(u'\n', QChar::LineSeparator);
    return text;
}

int endOf(const QTextBlock &block)
{
    return block.position() + block.length() - 1;
}

} // namespace

HighlightDocument::HighlightDocument(QObject *parent)
    : QObject(parent)
{
    // Once for all that changes together.
    m_pending.setSingleShot(true);
    m_pending.setInterval(0);
    connect(&m_pending, &QTimer::timeout, this, &HighlightDocument::rebuild);
    connect(this, &HighlightDocument::changed, &m_pending, qOverload<>(&QTimer::start));
}

int HighlightDocument::measure() const
{
    return measureChars;
}

qreal HighlightDocument::px(qreal pixels) const
{
    return pixels * m_scale;
}

void HighlightDocument::rebuild()
{
    // None while the text goes: theirs no longer is.
    if (!m_blocks.isEmpty() || m_titleEnd > 0) {
        m_blocks.clear();
        m_titleEnd = 0;
        emit built();
    }
    if (QTextDocument *document = m_textDocument ? m_textDocument->textDocument() : nullptr) {
        document->setUndoRedoEnabled(false);
        document->clear();
        QTextCursor cursor(document);
        cursor.beginEditBlock();
        if (!m_markdown.isEmpty())
            writeMarkdown(cursor);
        else if (!m_content.isEmpty())
            writeDocument(cursor);
        cursor.endEditBlock();
    }
    emit built();
}

void HighlightDocument::writeDocument(QTextCursor &cursor)
{
    const qreal size = m_font.pointSizeF() > 0 ? m_font.pointSizeF() : 11;
    const auto chars = [&](qreal scale, QFont::Weight weight, const QColor &color) {
        QTextCharFormat format;
        format.setFontFamilies({ m_font.family() });
        format.setFontPointSize(size * scale);
        format.setFontWeight(weight);
        format.setForeground(color);
        return format;
    };
    bool first = true;
    // Each paragraph a block, `above` it, `left` and `right` its margins.
    const auto paragraph = [&](const QString &text, qreal above, qreal left, qreal right, qreal lineHeight,
                               const QTextCharFormat &format) {
        QTextBlockFormat block;
        block.setTopMargin(px(above));
        block.setLeftMargin(px(left));
        block.setRightMargin(px(right));
        if (lineHeight > 0)
            block.setLineHeight(lineHeight * 100, QTextBlockFormat::ProportionalHeight);
        if (first)
            cursor.setBlockFormat(block);
        else
            cursor.insertBlock(block, format);
        first = false;
        cursor.insertText(text, format);
        return cursor.block();
    };
    const auto decoration = [this](const QString &kind, int start, int end, qreal left, qreal right, qreal outset) {
        m_blocks.append(QVariantMap{ { QStringLiteral("kind"), kind }, { QStringLiteral("start"), start },
                                     { QStringLiteral("end"), end }, { QStringLiteral("left"), px(left) },
                                     { QStringLiteral("right"), px(right) }, { QStringLiteral("outset"), px(outset) } });
    };

    const QTextCharFormat body = chars(1, QFont::Normal, m_textColor);
    // Above it, the TextEdit's padding: the document's first margin is not.
    m_titleEnd = endOf(paragraph(m_content.value(QStringLiteral("title")).toString(), 0, sideMargin, sideMargin,
                                 1.15, chars(1.75, QFont::ExtraBold, m_textColor)));
    const QString authors = m_content.value(QStringLiteral("authors")).toString();
    if (!authors.isEmpty())
        paragraph(authors, 4, sideMargin, sideMargin, 0, chars(1.1, QFont::Normal, m_mutedColor));
    const int meta = endOf(paragraph(m_content.value(QStringLiteral("counts")).toString(), 6, sideMargin, sideMargin, 0,
                                     chars(0.85, QFont::Normal, m_mutedColor)));
    decoration(QStringLiteral("rule"), meta, meta, sideMargin, sideMargin, ruleGap);

    // Below the rule, the first chapter no further.
    qreal above = 2 * ruleGap + 1;
    bool afterNote = false;
    for (const QVariant &chapterValue : m_content.value(QStringLiteral("chapters")).toList()) {
        const QVariantMap chapter = chapterValue.toMap();
        paragraph(chapter.value(QStringLiteral("title")).toString(), above, sideMargin, sideMargin, 1.25,
                  chars(1.3, QFont::Bold, m_textColor));
        above = headingGap;
        for (const QVariant &entryValue : chapter.value(QStringLiteral("entries")).toList()) {
            const QVariantMap entry = entryValue.toMap();
            const QStringList paragraphs = entry.value(QStringLiteral("paragraphs")).toStringList();
            const QString note = entry.value(QStringLiteral("note")).toString();
            if (!paragraphs.isEmpty()) {
                int start = -1;
                int end = 0;
                for (const QString &text : paragraphs) {
                    const QTextBlock block = paragraph(text, start < 0 ? above : paragraphGap,
                                                       sideMargin + quoteBarWidth + quoteIndent, sideMargin, 1.5, body);
                    if (start < 0)
                        start = block.position();
                    end = endOf(block);
                }
                decoration(QStringLiteral("quote"), start, end, sideMargin, 0, 0);
                above = pairGap;
            }
            afterNote = !note.isEmpty();
            if (afterNote) {
                const qreal inset = sideMargin + notePadding;
                const QTextBlock label = paragraph(tr("Note"), above + noteVPadding, inset, inset, 0,
                                                   chars(0.82, QFont::Bold, m_accentColor));
                const QTextBlock text = paragraph(softLines(note), 4, inset, inset, 1.45, body);
                decoration(QStringLiteral("note"), label.position(), endOf(text), sideMargin, sideMargin, noteVPadding);
            }
            above = entryGap + (afterNote ? noteVPadding : 0);
        }
        above = chapterGap + (afterNote ? noteVPadding : 0);
    }
}

void HighlightDocument::writeMarkdown(QTextCursor &cursor)
{
    QTextCharFormat plain;
    plain.setFontFamilies({ m_monoFont.family() });
    plain.setFontPointSize(m_monoFont.pointSizeF() > 0 ? m_monoFont.pointSizeF() : 10);
    plain.setForeground(m_textColor);
    QTextCharFormat mark = plain;
    mark.setForeground(m_faintColor);
    QTextCharFormat heading = plain;
    heading.setFontWeight(QFont::Bold);
    heading.setFontPointSize(plain.fontPointSize() * 1.333);
    QTextCharFormat headingMark = heading;
    headingMark.setForeground(m_faintColor);

    const QStringList lines = m_markdown.split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        const bool isHeading = line.startsWith(QLatin1String("## "));
        QTextBlockFormat block;
        block.setLeftMargin(px(sourceSideMargin));
        block.setRightMargin(px(sourceSideMargin));
        block.setTopMargin(i > 0 && isHeading ? px(6) : 0);
        block.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
        if (i == 0)
            cursor.setBlockFormat(block);
        else
            cursor.insertBlock(block, plain);
        // The marks dimmed, the text as it is.
        if (isHeading) {
            cursor.insertText(line.left(3), headingMark);
            cursor.insertText(line.mid(3), heading);
        } else if (line == QLatin1String(">")) {
            cursor.insertText(line, mark);
        } else if (line.startsWith(QLatin1String("> "))) {
            cursor.insertText(line.left(2), mark);
            cursor.insertText(line.mid(2), plain);
        } else {
            cursor.insertText(line, plain);
        }
    }
}
