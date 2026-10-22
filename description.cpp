#include "description.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>

namespace Description {

namespace {

// The characters a text's marks are made of, which a backslash makes
// plain.
const QString marks = QStringLiteral("\\`*_{}[]()#+-.!>");

// The first private-use character, which stands for a backslashed one while
// the marks are read.
constexpr char16_t escapeBase = 0xE000;

QString htmlEscaped(const QString &text)
{
    return text.toHtmlEscaped();
}

// What a line of text holds: bold, italics and links, the characters a
// backslash escapes kept as they are.
QString inlineHtml(QString line)
{
    // A backslashed mark as a private-use character until the end.
    for (qsizetype i = 0; i + 1 < line.size(); ++i) {
        if (line.at(i) == u'\\' && marks.contains(line.at(i + 1)))
            line.replace(i, 2, QChar(char16_t(escapeBase + marks.indexOf(line.at(i + 1)))));
    }
    line = htmlEscaped(line);

    static const QRegularExpression link(QStringLiteral("\\[([^\\]]*)\\]\\(([^)\\s]+)\\)"));
    line.replace(link, QStringLiteral("<a href=\"\\2\">\\1</a>"));
    static const QRegularExpression bold(QStringLiteral("(\\*\\*|__)(?=\\S)(.+?)(?<=\\S)\\1"));
    line.replace(bold, QStringLiteral("<strong>\\2</strong>"));
    static const QRegularExpression starItalic(QStringLiteral("\\*(?=\\S)([^*]+?)(?<=\\S)\\*"));
    line.replace(starItalic, QStringLiteral("<em>\\1</em>"));
    // An underscore inside a word is part of it.
    static const QRegularExpression underscoreItalic(
        QStringLiteral("(?<![\\p{L}\\p{N}])_(?=\\S)([^_]+?)(?<=\\S)_(?![\\p{L}\\p{N}])"),
        QRegularExpression::UseUnicodePropertiesOption);
    line.replace(underscoreItalic, QStringLiteral("<em>\\1</em>"));

    for (qsizetype i = 0; i < line.size(); ++i) {
        const char16_t c = line.at(i).unicode();
        if (c >= escapeBase && c < escapeBase + marks.size())
            line.replace(i, 1, htmlEscaped(QString(marks.at(c - escapeBase))));
    }
    return line;
}

// A run of a block's text with one formatting.
struct Run
{
    QString text;
    bool bold = false;
    bool italic = false;
    QString href;
};

// Escapes what the text would read as a mark: backslashes, stars,
// underscores and brackets anywhere.
QString escapedText(const QString &text)
{
    QString result;
    result.reserve(text.size());
    for (const QChar c : text) {
        if (c == u'\\' || c == u'*' || c == u'_' || c == u'[' || c == u']')
            result += u'\\';
        result += c;
    }
    return result;
}

// One line of runs as text with its marks: links outermost, then bold, then
// italics, the spaces at a run's ends left outside them.
QString markedLine(const QList<Run> &runs)
{
    QString line;
    for (const Run &run : runs) {
        const QString trimmed = run.text.trimmed();
        if (trimmed.isEmpty()) {
            line += run.text;
            continue;
        }
        const qsizetype lead = run.text.indexOf(trimmed);
        QString marked = escapedText(trimmed);
        if (run.italic)
            marked = run.bold ? u'_' + marked + u'_' : u'*' + marked + u'*';
        if (run.bold)
            marked = QStringLiteral("**") + marked + QStringLiteral("**");
        if (!run.href.isEmpty())
            marked = u'[' + marked + QStringLiteral("](") + QString(run.href).replace(u' ', QStringLiteral("%20")) + u')';
        line += run.text.left(lead) + marked + run.text.mid(lead + trimmed.size());
    }
    // What would start a heading or a list item is plain.
    static const QRegularExpression blockMark(QStringLiteral("^(\\s*)(#|[-+]\\s|\\d+[.)]\\s)"));
    const QRegularExpressionMatch match = blockMark.match(line);
    if (match.hasMatch())
        line.insert(match.capturedStart(2) + (match.captured(2).startsWith(u'#') ? 0 : match.capturedLength(2) - 2), u'\\');
    return line;
}

// The block's lines: its runs, split where a line ends inside it. A
// heading's text is bold by its nature, not by a mark.
QStringList blockLines(const QTextBlock &block, bool heading)
{
    QList<QList<Run>> lines(1);
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        const QTextCharFormat format = fragment.charFormat();
        Run run;
        run.bold = !heading && format.fontWeight() >= QFont::DemiBold;
        run.italic = format.fontItalic();
        run.href = format.isAnchor() ? format.anchorHref() : QString();
        const QStringList parts = fragment.text().split(QChar::LineSeparator);
        for (qsizetype i = 0; i < parts.size(); ++i) {
            if (i > 0)
                lines.append(QList<Run>());
            run.text = parts.at(i);
            QList<Run> &current = lines.last();
            // The same formatting goes on.
            if (!current.isEmpty() && current.last().bold == run.bold && current.last().italic == run.italic
                && current.last().href == run.href)
                current.last().text += run.text;
            else if (!run.text.isEmpty())
                current.append(run);
        }
    }
    QStringList result;
    for (const QList<Run> &runs : std::as_const(lines))
        result.append(markedLine(runs).trimmed());
    return result;
}

} // namespace

QString cleaned(const QString &html)
{
    if (html.trimmed().isEmpty())
        return {};
    if (!html.contains(u'<')) {
        QStringList paragraphs;
        static const QRegularExpression blankLines(QStringLiteral("\\n\\s*\\n"));
        for (const QString &paragraph : html.split(blankLines))
            paragraphs.append(QStringLiteral("<p>%1</p>")
                                  .arg(paragraph.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"))));
        return paragraphs.join(QString());
    }
    static const QRegularExpression attributes(
        QStringLiteral("\\s(style|face|color|size|class|bgcolor|width|height|align|dir|lang)"
                       "\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression wrappers(QStringLiteral("</?(font|span)\\b[^>]*>"),
                                             QRegularExpression::CaseInsensitiveOption);
    QString result = html;
    result.remove(attributes);
    result.remove(wrappers);
    return result.trimmed();
}

QString toText(const QString &html)
{
    if (html.trimmed().isEmpty())
        return {};
    QTextDocument document;
    document.setHtml(cleaned(html));
    QStringList blocks;
    const QTextList *previousList = nullptr;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const int level = block.blockFormat().headingLevel();
        QStringList lines = blockLines(block, level > 0);
        while (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        while (!lines.isEmpty() && lines.first().isEmpty())
            lines.removeFirst();
        const QTextList *list = block.textList();
        if (lines.isEmpty()) {
            previousList = nullptr;
            continue;
        }
        QString text = lines.join(u'\n');
        if (list) {
            const QTextListFormat::Style style = list->format().style();
            const bool ordered = style == QTextListFormat::ListDecimal || style == QTextListFormat::ListLowerAlpha
                || style == QTextListFormat::ListUpperAlpha || style == QTextListFormat::ListLowerRoman
                || style == QTextListFormat::ListUpperRoman;
            text = (ordered ? QStringLiteral("%1. ").arg(list->itemNumber(block) + 1) : QStringLiteral("- ")) + text;
            // An item's next lines go under it.
            text.replace(u'\n', QStringLiteral("\n  "));
        } else if (level > 0) {
            text = QString(level, u'#') + u' ' + text.replace(u'\n', u' ');
        }
        // Items of one list follow each other; anything else stands apart.
        if (list && list == previousList)
            blocks.last() += u'\n' + text;
        else
            blocks.append(text);
        previousList = list;
    }
    return blocks.join(QStringLiteral("\n\n"));
}

QString toHtml(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(u'\r', u'\n');
    QStringList blocks;
    QStringList paragraph;
    QStringList items;
    QString listTag;
    int listStart = 1;

    const auto flushParagraph = [&] {
        if (!paragraph.isEmpty())
            blocks.append(QStringLiteral("<p>%1</p>").arg(paragraph.join(QStringLiteral("<br>"))));
        paragraph.clear();
    };
    const auto flushList = [&] {
        if (!items.isEmpty()) {
            const QString start = listTag == QLatin1String("ol") && listStart != 1
                ? QStringLiteral(" start=\"%1\"").arg(listStart) : QString();
            blocks.append(QStringLiteral("<%1%2>%3</%1>").arg(listTag, start, items.join(QString())));
        }
        items.clear();
        listTag.clear();
    };

    static const QRegularExpression heading(QStringLiteral("^\\s*(#{1,6})\\s+(.*?)\\s*#*\\s*$"));
    static const QRegularExpression bullet(QStringLiteral("^\\s*[-*+]\\s+(.*)$"));
    static const QRegularExpression ordered(QStringLiteral("^\\s*(\\d+)[.)]\\s+(.*)$"));
    for (const QString &line : normalized.split(u'\n')) {
        if (line.trimmed().isEmpty()) {
            flushParagraph();
            flushList();
            continue;
        }
        if (const QRegularExpressionMatch match = heading.match(line); match.hasMatch()) {
            flushParagraph();
            flushList();
            const int level = int(match.capturedLength(1));
            blocks.append(QStringLiteral("<h%1>%2</h%1>").arg(level).arg(inlineHtml(match.captured(2))));
            continue;
        }
        const QRegularExpressionMatch item = bullet.match(line);
        const QRegularExpressionMatch number = item.hasMatch() ? QRegularExpressionMatch() : ordered.match(line);
        if (item.hasMatch() || number.hasMatch()) {
            flushParagraph();
            const QString tag = item.hasMatch() ? QStringLiteral("ul") : QStringLiteral("ol");
            if (tag != listTag) {
                flushList();
                listTag = tag;
                listStart = number.hasMatch() ? number.captured(1).toInt() : 1;
            }
            items.append(QStringLiteral("<li>%1</li>").arg(inlineHtml(item.hasMatch() ? item.captured(1)
                                                                                        : number.captured(2))));
            continue;
        }
        // A line under a list item goes on with it.
        if (!items.isEmpty()) {
            items.last().insert(items.last().size() - 5, QStringLiteral("<br>") + inlineHtml(line.trimmed()));
            continue;
        }
        paragraph.append(inlineHtml(line.trimmed()));
    }
    flushParagraph();
    flushList();
    return blocks.isEmpty() ? QString() : QStringLiteral("<div>%1</div>").arg(blocks.join(QString()));
}

bool losesFormatting(const QString &html)
{
    static const QSet<QString> kept = {
        QStringLiteral("html"), QStringLiteral("head"), QStringLiteral("body"), QStringLiteral("meta"),
        QStringLiteral("div"), QStringLiteral("p"), QStringLiteral("br"), QStringLiteral("b"),
        QStringLiteral("strong"), QStringLiteral("i"), QStringLiteral("em"), QStringLiteral("u"),
        QStringLiteral("a"), QStringLiteral("ul"), QStringLiteral("ol"), QStringLiteral("li"),
        QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"),
        QStringLiteral("h5"), QStringLiteral("h6"), QStringLiteral("span"), QStringLiteral("font"),
    };
    static const QRegularExpression tag(QStringLiteral("<\\s*([A-Za-z][A-Za-z0-9]*)"));
    for (QRegularExpressionMatchIterator it = tag.globalMatch(html); it.hasNext();) {
        if (!kept.contains(it.next().captured(1).toLower()))
            return true;
    }
    return false;
}

bool noteLosesFormatting(const QString &html)
{
    static const QRegularExpression styled(QStringLiteral(
        "<\\s*(u|s|strike|del|ins|sup|sub|font|code|pre|blockquote|hr)\\b|\\b(style|align|color)\\s*="),
                                           QRegularExpression::CaseInsensitiveOption);
    return losesFormatting(html) || styled.match(html).hasMatch();
}

} // namespace Description
