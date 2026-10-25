#pragma once

#include <QList>
#include <QMutex>
#include <QString>

#include <memory>

class EpubBook;

namespace XmlTree {
struct Node;
}

// A search through an EPUB's text as the reader shows it: the texts of each
// spine item's body, case and accents ignored as the library's search
// ignores them, white space as one space; each place found by the CFIs the
// reader's script would give it, within its spine item.
class BookSearch
{
public:
    struct Match
    {
        int spineIndex = -1;
        // "/2/4/1:3", as the reader's highlights keep theirs.
        QString start;
        QString end;
        // The words found, and some around them.
        QString text;
        QString before;
        QString after;
        // The title of the table of contents entry it is under.
        QString chapter;
    };

    explicit BookSearch(std::shared_ptr<const EpubBook> book);
    ~BookSearch();

    // Every place of `query` in the book, in its order, at most `limit`;
    // `more` tells whether there were more. Safe from any thread, one search
    // at a time; each spine item's text is read once, the first search
    // reading the book.
    QList<Match> find(const QString &query, int limit = 1000, bool *more = nullptr);

private:
    struct Item;
    std::shared_ptr<const EpubBook> m_book;
    QMutex m_lock;
    QList<std::shared_ptr<Item>> m_items;
};

// The sentences of a spine item, as they are read aloud: Qt's sentence
// boundaries within each block, a paragraph or a heading never run into the
// next; each with its CFIs within the item, and where it is in the page's
// text, for the words read in it.
class BookSentences
{
public:
    struct Sentence
    {
        QString start;
        QString end;
        QString text;
        // Its first character in the page's text.
        qsizetype at = 0;
    };

    // The sentences of the spine item `spineIndex`; none for one that
    // cannot be read.
    BookSentences(const EpubBook &book, int spineIndex);
    ~BookSentences();

    const QList<Sentence> &sentences() const { return m_sentences; }
    // The sentence a CFI within the item is in, or the first after it; 0
    // for one that names no place.
    int sentenceAt(const QString &cfi) const;
    // The CFI of the character `at` of the page's text.
    QString cfiAt(qsizetype at) const;

private:
    struct Page;
    std::unique_ptr<Page> m_page;
    QList<Sentence> m_sentences;
};

