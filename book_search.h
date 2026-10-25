#pragma once

#include <QList>
#include <QMutex>
#include <QString>

#include <memory>

class EpubBook;

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
