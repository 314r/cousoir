#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "book.h"
#include "book_edit.h"
#include "calibre_text.h"
#include "commit_engine.h"

// What a library collects over time and Cousoir offers to tidy: the same
// book twice, one author under several names, books whose files are gone and
// files the database does not know. The files outside the database are
// found as Calibre's Check library finds them, scan_library() and
// process_book() of src/calibre/library/check_library.py; the rest is
// Cousoir's, in the spirit of the Find Duplicates plugin.
namespace LibraryCheck {

struct Options
{
    CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    QString interfaceLanguage;
    // Whether to walk the library's folder for files outside the database.
    bool scanFiles = true;
    // Asked between folders: true stops the walk.
    std::function<bool()> canceled;
};

// Books that look like copies of one book, the one to keep first.
struct DuplicateGroup
{
    QList<int> books;
    // Same title and authors, the authors' variants aside.
    bool sameTitle = false;
    // The ISBN-13s the copies share.
    QStringList isbns;
};

// One author under several names, by author id.
struct AuthorGroup
{
    QList<int> authors;
    // Joined through initials, "B. Vian" for "Boris Vian": possibly the same
    // person rather than surely.
    bool initials = false;
};

struct MissingFiles
{
    int book = 0;
    // The formats the database lists whose file is gone.
    QStringList formats;
    bool noFormats = false;
    bool folderMissing = false;
};

// A file or folder in the library's folder the database does not know,
// relative to the library.
struct OutsideFile
{
    QString path;
    bool folder = false;
    // For a folder named like a book's, "Title (12)": its number.
    int book = 0;
    qint64 size = 0;
};

struct Findings
{
    QList<DuplicateGroup> duplicates;
    QList<AuthorGroup> authorVariants;
    QList<MissingFiles> missing;
    QList<OutsideFile> outside;
    // Author names differing only by case: no author can be written until
    // Calibre merges them.
    QStringList caseDuplicates;
    // Folders that could not be read, relative to the library.
    QStringList unreadable;
    // Whether the folder was walked to the end.
    bool scanned = false;

    qsizetype count() const;
};

Findings check(const LibraryData &data, const Options &options);

// The words of a name, folded: lower case, no accents, no punctuation.
QStringList nameWords(const QString &name);
// A name with its words in order, "boris vian" for "Vian, Boris".
QString authorKey(const QString &name);
// Whether two names are one through initials: the same surname and as many
// given names, each the same or an initial of the other.
bool sameByInitials(const QString &first, const QString &second, const CalibreText::Tweaks &tweaks);
// An ISBN as its ISBN-13, empty when it is not a valid one.
QString isbnKey(const QString &value);

// What a finding is ignored under: its kind and what it is about, so that
// it shows again once that changes.
QString key(const DuplicateGroup &group);
QString key(const AuthorGroup &group);
QString key(const MissingFiles &missing);
QString key(const OutsideFile &file);
// The findings ignored in the library of id `uuid`, kept in the settings.
QStringList ignoredKeys(const QString &uuid);
void ignore(const QString &uuid, const QString &key);
void clearIgnored(const QString &uuid);

// The change that keeps `keep` and moves `others` to the trash, with the
// formats it has no file for copied from them first.
bool keepOne(const LibraryData &data, int keep, const QList<int> &others, CommitEngine::Request *request,
             QString *error);
// The edits that file the books of the authors `authorIds` under the name of
// `keepAuthorId`, in place, each name once.
QList<BookEdit> mergeAuthors(const LibraryData &data, const QList<int> &authorIds, int keepAuthorId,
                             QString *error);
// Whether the book's author sort is its authors' sorts, which an author
// change carries along, as Calibre's editor does.
bool authorSortFollows(const LibraryData &data, const Book &book);

} // namespace LibraryCheck
