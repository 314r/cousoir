#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

#include <memory>
#include <vector>

#include "kf8_index.h"

// A KF8 book's text as Calibre's KF8 writer cuts it
// (src/calibre/ebooks/mobi/writer8/skeleton.py): each page a skeleton, its
// markup without the body's content, and chunks of that content of at most
// 8 KB, which the Kindle puts back into the skeleton where the chunk table
// says.
namespace Kf8Skeleton {

// An element as lxml keeps one, without namespaces: its text before its
// first child, its tail after its end.
struct Element
{
    QString name;
    QList<QPair<QString, QString>> attributes;
    QString text;
    QString tail;
    std::vector<std::unique_ptr<Element>> children;
    Element *parent = nullptr;

    QString attribute(const QString &key) const;
    bool hasAttribute(const QString &key) const;
    void setAttribute(const QString &key, const QString &value);
    void removeAttribute(const QString &key);
    Element *append(std::unique_ptr<Element> child);
};

// The tags kindlegen gives an aid to.
bool aidAble(const QString &name);

// The element as lxml writes it, a tag Kindle needs closed never written as
// one alone (close_self_closing_tags()).
QByteArray serialize(const Element &element, bool withTail);

struct Text
{
    // The skeletons and their chunks, with the links set.
    QByteArray text;
    QList<Kf8Index::Skeleton> skeletons;
    QList<Kf8Index::Chunk> chunks;
    // By aid: its chunk, where it is from that chunk's start, and in the
    // text with the chunks in their skeletons.
    struct Place
    {
        int position = 0;
        int offset = 0;
        quint32 textOffset = 0;
    };
    QHash<QString, Place> places;
};

// Chunker: `pages`, each an <html> whose tags have their aid last, their
// content taken into chunks; `links`, the aid each link placeholder of
// their hrefs goes to.
Text chunk(std::vector<std::unique_ptr<Element>> &pages, const QHash<QString, QString> &links);

} // namespace Kf8Skeleton
