#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "book.h"
#include "opf.h"

// Books saved out of the library, as Calibre's Save to disk saves them
// (src/calibre/library/save_to_disk.py): each book's files under a path its
// template makes, "{author_sort}/{title}/{title} - {authors}", with its
// cover and an OPF beside them. Unlike Calibre, the files are copied as
// they are: Cousoir does not write the library's metadata into them.
namespace SaveToDisk {

// Calibre's options, from save_to_disk.py.json.
struct Options
{
    QString templateText = QStringLiteral("{author_sort}/{title}/{title} - {authors}");
    bool asciiize = false;
    bool toLowercase = false;
    bool replaceWhitespace = false;
    bool singleDir = false;
    bool saveCover = true;
    bool writeOpf = true;
    // "all", or formats separated by commas.
    QString formats = QStringLiteral("all");

    static Options load(const QString &calibreConfigDirectory);
};

// get_path_components(): where the book's files go under the root, the
// last part their name without its extension; `length` what the path may
// take after the root.
QStringList components(const LibraryData &data, const Book &book, const Options &options, qsizetype length,
                       const QString &interfaceLanguage);

struct Outcome
{
    int saved = 0;
    // "Title: why", for each book not saved.
    QStringList failures;
};

// save_to_disk(): the books of `bookIds` saved under `root`; `progress`
// hears how many are done, and stops it by returning false. `opf` says who
// writes the OPFs.
Outcome save(const LibraryData &data, const QList<int> &bookIds, const QString &root, const Options &options,
             const Opf::Settings &opf, const std::function<bool(int)> &progress = {});

} // namespace SaveToDisk
