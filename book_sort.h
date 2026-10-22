#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

struct CalibreConfig;
struct LibraryData;

// How Calibre's book list sorts the books: Cache.multisort() on the sort
// keys each field makes of its values (db/fields.py). A sort is a list of
// fields and directions, the most significant first, as the list keeps the
// sorts asked for; books alike in all keep the order they had.
namespace BookSort {

struct Level
{
    // Calibre's name of the field, "title", "timestamp", "#genre"…
    QString field;
    bool ascending = true;

    bool operator==(const Level &) const = default;
};

// The fields the list sorts by: Calibre's built-in ones and the library's
// custom columns, by search term.
QStringList fields(const LibraryData &data);
bool sortable(const LibraryData &data, const QString &field);

// The sort keys of a library's books, made for a field when first asked and
// kept. It keeps its own copy of the books, which Qt shares with theirs;
// made on one thread, it may be handed to another, not used by two at once.
class Sorter
{
public:
    Sorter(const LibraryData &data, const CalibreConfig &config);
    ~Sorter();

    // Makes the keys of `fields` now, ahead of the sorts that need them.
    void prepare(const QStringList &fields) const;

    // `rows`, indexes into the library's books, sorted by `levels`; equal
    // ones stay in the order given. A field that is no field sorts nothing.
    void sort(const QList<Level> &levels, QList<qsizetype> *rows) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// The sort history after sorting by `level`: it first, then those before
// but the same field, `maximum` of them at most (maximum_resort_levels).
QList<Level> addToHistory(const QList<Level> &history, const Level &level, int maximum);

} // namespace BookSort
