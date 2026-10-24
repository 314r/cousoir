#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class QTextCursor;

// A book's highlights typeset into a TextEdit as kbextract2's document.go
// lays them out: the title, the author and the counts over a rule, then by
// chapter each highlight a quote, its note a card under it. The page draws
// the quote bars, the cards and the rule under the text, from `blocks`.
// With `markdown`, that text as kbextract2's readerview.go shows it
// instead: in the fixed font, its headings bold, its marks dimmed.
class HighlightDocument : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *textDocument MEMBER m_textDocument NOTIFY changed)
    // HighlightBrowser's document.
    Q_PROPERTY(QVariantMap content MEMBER m_content NOTIFY changed)
    Q_PROPERTY(QString markdown MEMBER m_markdown NOTIFY changed)
    Q_PROPERTY(QFont font MEMBER m_font NOTIFY changed)
    Q_PROPERTY(QFont monoFont MEMBER m_monoFont NOTIFY changed)
    // Of kbextract2's pixels, as Theme.spacingScale.
    Q_PROPERTY(qreal scale MEMBER m_scale NOTIFY changed)
    Q_PROPERTY(QColor textColor MEMBER m_textColor NOTIFY changed)
    Q_PROPERTY(QColor mutedColor MEMBER m_mutedColor NOTIFY changed)
    Q_PROPERTY(QColor faintColor MEMBER m_faintColor NOTIFY changed)
    Q_PROPERTY(QColor accentColor MEMBER m_accentColor NOTIFY changed)
    // { kind, start, end, left, right, outset } of each decoration: a
    // "quote" bar, a "note" card or the "rule", from the line of text
    // position `start` to that of `end`; `left` and `right` from the page's
    // sides, a bar `left` alone; `outset` above and below a card, below its
    // line for the rule.
    Q_PROPERTY(QVariantList blocks MEMBER m_blocks NOTIFY built)
    // Where the title ends, to tell when it has scrolled away.
    Q_PROPERTY(int titleEnd MEMBER m_titleEnd NOTIFY built)
    // kbextract2's measure of a line, in average characters.
    Q_PROPERTY(int measure READ measure CONSTANT)

public:
    explicit HighlightDocument(QObject *parent = nullptr);

    int measure() const;

signals:
    void changed();
    void built();

private:
    void rebuild();
    void writeDocument(QTextCursor &cursor);
    void writeMarkdown(QTextCursor &cursor);
    qreal px(qreal pixels) const;

    QPointer<QQuickTextDocument> m_textDocument;
    QVariantMap m_content;
    QString m_markdown;
    QFont m_font;
    QFont m_monoFont;
    qreal m_scale = 1;
    QColor m_textColor;
    QColor m_mutedColor;
    QColor m_faintColor;
    QColor m_accentColor;
    QVariantList m_blocks;
    int m_titleEnd = 0;
    QTimer m_pending;
};
