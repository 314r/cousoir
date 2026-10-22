#include "library_check.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

#include <algorithm>
#include <numeric>
#include <optional>

#include "concurrency_guard.h"

namespace {

// Union-find over indexes: each set is named by its smallest index.
class Sets
{
public:
    explicit Sets(qsizetype size)
        : m_parent(size)
    {
        std::iota(m_parent.begin(), m_parent.end(), qsizetype(0));
    }

    qsizetype find(qsizetype index)
    {
        while (m_parent[index] != index) {
            m_parent[index] = m_parent[m_parent[index]];
            index = m_parent[index];
        }
        return index;
    }

    void join(qsizetype first, qsizetype second)
    {
        first = find(first);
        second = find(second);
        if (first != second)
            m_parent[std::max(first, second)] = std::min(first, second);
    }

private:
    QList<qsizetype> m_parent;
};

// "<library id>|<key>" for the findings the user ignored.
const char ignoredSetting[] = "Library/ignoredFindings";

QString idList(QList<int> ids)
{
    std::sort(ids.begin(), ids.end());
    QStringList texts;
    for (const int id : std::as_const(ids))
        texts.append(QString::number(id));
    return texts.join(u',');
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

// A name split at the comma of its author sort, "Vian, Boris": the surname's
// words and the given names'.
struct NameParts
{
    QStringList surname;
    QStringList given;
};

std::optional<NameParts> nameParts(const QString &name, const CalibreText::Tweaks &tweaks)
{
    const QString sort = CalibreText::authorToAuthorSort(name, tweaks, QStringLiteral("comma"));
    const qsizetype comma = sort.indexOf(u',');
    if (comma < 0)
        return std::nullopt;
    NameParts parts{ LibraryCheck::nameWords(sort.left(comma)), LibraryCheck::nameWords(sort.mid(comma + 1)) };
    if (parts.surname.isEmpty() || parts.given.isEmpty())
        return std::nullopt;
    return parts;
}

bool hasInitial(const NameParts &parts)
{
    return std::any_of(parts.given.cbegin(), parts.given.cend(), [](const QString &word) { return word.size() == 1; });
}

int presentFormats(const Book &book)
{
    return int(std::count_if(book.formats.cbegin(), book.formats.cend(),
                             [](const BookFormat &format) { return !format.file.isEmpty(); }));
}

// What the author grouping gives the book grouping: the group of each author.
struct Authors
{
    QList<LibraryCheck::AuthorGroup> groups;
    QHash<int, qsizetype> groupOf;
};

Authors groupAuthors(const QList<FacetItem> &authors, const CalibreText::Tweaks &tweaks)
{
    // The same name written differently: one node per key.
    QList<QList<qsizetype>> nodes;
    QHash<QString, qsizetype> byKey;
    for (qsizetype i = 0; i < authors.size(); ++i) {
        const QString key = LibraryCheck::authorKey(authors.at(i).name);
        if (key.isEmpty() || !byKey.contains(key)) {
            if (!key.isEmpty())
                byKey.insert(key, nodes.size());
            nodes.append(QList<qsizetype>{ i });
        } else {
            nodes[byKey.value(key)].append(i);
        }
    }

    // Through initials: a node holding "B. Vian" joins the one node of the
    // same surname it fits, and none when it fits several, "J. Smith" between
    // "John Smith" and "Jane Smith".
    QList<std::optional<NameParts>> parts;
    parts.reserve(authors.size());
    for (const FacetItem &author : authors)
        parts.append(nameParts(author.name, tweaks));
    QHash<QString, QList<qsizetype>> bySurname;
    for (qsizetype node = 0; node < nodes.size(); ++node) {
        for (const qsizetype author : std::as_const(nodes.at(node))) {
            if (!parts.at(author))
                continue;
            QList<qsizetype> &list = bySurname[parts.at(author)->surname.join(u' ')];
            if (!list.contains(node))
                list.append(node);
        }
    }
    Sets sets(nodes.size());
    QList<qsizetype> linked;
    for (qsizetype node = 0; node < nodes.size(); ++node) {
        QSet<qsizetype> fitting;
        for (const qsizetype author : std::as_const(nodes.at(node))) {
            if (!parts.at(author) || !hasInitial(*parts.at(author)))
                continue;
            for (const qsizetype other : bySurname.value(parts.at(author)->surname.join(u' '))) {
                if (other == node)
                    continue;
                for (const qsizetype candidate : std::as_const(nodes.at(other))) {
                    if (LibraryCheck::sameByInitials(authors.at(author).name, authors.at(candidate).name, tweaks))
                        fitting.insert(other);
                }
            }
        }
        if (fitting.size() == 1) {
            sets.join(node, *fitting.cbegin());
            linked.append(node);
        }
    }

    Authors result;
    QMap<qsizetype, QList<qsizetype>> members;
    for (qsizetype node = 0; node < nodes.size(); ++node) {
        const qsizetype root = sets.find(node);
        members[root] += nodes.at(node);
        for (const qsizetype author : std::as_const(nodes.at(node)))
            result.groupOf.insert(authors.at(author).id, root);
    }
    QSet<qsizetype> throughInitials;
    for (const qsizetype node : std::as_const(linked))
        throughInitials.insert(sets.find(node));
    for (auto it = members.cbegin(); it != members.cend(); ++it) {
        if (it.value().size() < 2)
            continue;
        QList<qsizetype> ordered = it.value();
        // The name most books use first, the one to keep most likely.
        std::sort(ordered.begin(), ordered.end(), [&authors](qsizetype a, qsizetype b) {
            if (authors.at(a).count != authors.at(b).count)
                return authors.at(a).count > authors.at(b).count;
            return authors.at(a).name < authors.at(b).name;
        });
        LibraryCheck::AuthorGroup group;
        for (const qsizetype author : std::as_const(ordered))
            group.authors.append(authors.at(author).id);
        group.initials = throughInitials.contains(it.key());
        result.groups.append(group);
    }
    return result;
}

QList<LibraryCheck::DuplicateGroup> groupBooks(const LibraryData &data, const Authors &authors,
                                               const LibraryCheck::Options &options)
{
    const QList<Book> &books = data.books;
    Sets sets(books.size());
    QSet<qsizetype> byTitle;

    // The same title and authors, as find_identical_books() of db/utils.py
    // matches them, the authors' variants aside: two books in different
    // languages are two books.
    const QString unknown = CalibreText::unknownText(options.interfaceLanguage);
    QHash<QString, QList<qsizetype>> buckets;
    for (qsizetype i = 0; i < books.size(); ++i) {
        const Book &book = books.at(i);
        if (book.title == unknown || book.title == QLatin1String("Unknown"))
            continue;
        const QString title =
            LibraryCheck::nameWords(CalibreText::fuzzyTitle(book.title, options.tweaks, options.interfaceLanguage))
                .join(u' ');
        if (title.isEmpty())
            continue;
        QStringList names;
        for (const int id : book.authorIds) {
            const QString name = authors.groupOf.contains(id) ? QString::number(authors.groupOf.value(id))
                                                              : QStringLiteral("a%1").arg(id);
            if (!names.contains(name))
                names.append(name);
        }
        names.sort();
        buckets[title + u'\x1f' + names.join(u',')].append(i);
    }
    for (const QList<qsizetype> &bucket : std::as_const(buckets)) {
        if (bucket.size() < 2)
            continue;
        QMap<QString, QList<qsizetype>> byLanguages;
        QList<qsizetype> none;
        for (const qsizetype i : bucket) {
            if (books.at(i).languages.isEmpty())
                none.append(i);
            else
                byLanguages[books.at(i).languages.join(u',')].append(i);
        }
        // A book of no language goes with the books of one language.
        if (byLanguages.size() == 1) {
            byLanguages.first() += none;
            none.clear();
        }
        QList<QList<qsizetype>> parts = byLanguages.values();
        parts.append(none);
        for (const QList<qsizetype> &part : std::as_const(parts)) {
            if (part.size() < 2)
                continue;
            for (const qsizetype i : part) {
                sets.join(part.first(), i);
                byTitle.insert(i);
            }
        }
    }

    // The same ISBN.
    QMap<QString, QList<qsizetype>> byIsbn;
    for (qsizetype i = 0; i < books.size(); ++i) {
        QSet<QString> own;
        for (const auto &identifier : books.at(i).identifiers) {
            if (identifier.first.compare(QLatin1String("isbn"), Qt::CaseInsensitive) != 0)
                continue;
            const QString isbn = LibraryCheck::isbnKey(identifier.second);
            if (!isbn.isEmpty() && !own.contains(isbn)) {
                own.insert(isbn);
                byIsbn[isbn].append(i);
            }
        }
    }
    for (const QList<qsizetype> &shared : std::as_const(byIsbn)) {
        for (const qsizetype i : shared)
            sets.join(shared.first(), i);
    }

    QMap<qsizetype, LibraryCheck::DuplicateGroup> groups;
    // The title of each group's first copy, folded as the groups sort by it.
    QHash<int, QString> titles;
    QMap<qsizetype, QList<qsizetype>> members;
    for (qsizetype i = 0; i < books.size(); ++i)
        members[sets.find(i)].append(i);
    for (auto it = members.cbegin(); it != members.cend(); ++it) {
        if (it.value().size() < 2)
            continue;
        LibraryCheck::DuplicateGroup &group = groups[it.key()];
        QList<qsizetype> ordered = it.value();
        // The copy with the most files first: the one to keep most likely.
        std::sort(ordered.begin(), ordered.end(), [&books](qsizetype a, qsizetype b) {
            const Book &first = books.at(a);
            const Book &second = books.at(b);
            if (presentFormats(first) != presentFormats(second))
                return presentFormats(first) > presentFormats(second);
            if (first.formats.size() != second.formats.size())
                return first.formats.size() > second.formats.size();
            if (first.timestamp != second.timestamp)
                return first.timestamp < second.timestamp;
            return first.id < second.id;
        });
        for (const qsizetype i : std::as_const(ordered)) {
            group.books.append(books.at(i).id);
            group.sameTitle = group.sameTitle || byTitle.contains(i);
        }
        titles.insert(group.books.first(), CalibreText::foldForSearch(books.at(ordered.first()).title));
    }
    for (auto it = byIsbn.cbegin(); it != byIsbn.cend(); ++it) {
        if (it.value().size() > 1)
            groups[sets.find(it.value().first())].isbns.append(it.key());
    }
    QList<LibraryCheck::DuplicateGroup> result = groups.values();
    std::sort(result.begin(), result.end(), [&titles](const LibraryCheck::DuplicateGroup &a,
                                                      const LibraryCheck::DuplicateGroup &b) {
        const QString first = titles.value(a.books.first());
        const QString second = titles.value(b.books.first());
        return first != second ? first < second : a.books.first() < b.books.first();
    });
    return result;
}

QList<LibraryCheck::MissingFiles> missingFiles(const LibraryData &data)
{
    QList<LibraryCheck::MissingFiles> result;
    for (const Book &book : data.books) {
        const QString folder = book.path.isEmpty() ? QString() : QDir(data.libraryPath).filePath(book.path);
        LibraryCheck::MissingFiles missing;
        missing.book = book.id;
        missing.folderMissing = folder.isEmpty() || !QFileInfo(folder).isDir();
        missing.noFormats = book.formats.isEmpty();
        for (const BookFormat &format : book.formats) {
            if (!format.file.isEmpty())
                continue;
            // Nextcloud and ownCloud leave a placeholder for a file they keep
            // online only: not missing, just not on this computer.
            const QString expected = QDir(folder).filePath(format.name + u'.' + format.format.toLower());
            if (!folder.isEmpty()
                && (QFileInfo::exists(expected + QStringLiteral(".nextcloud"))
                    || QFileInfo::exists(expected + QStringLiteral(".owncloud"))))
                continue;
            missing.formats.append(format.format);
        }
        if (missing.noFormats || missing.folderMissing || !missing.formats.isEmpty())
            result.append(missing);
    }
    return result;
}

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
constexpr Qt::CaseSensitivity fileNameCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity fileNameCase = Qt::CaseSensitive;
#endif

QString fileKey(const QString &name)
{
    return fileNameCase == Qt::CaseSensitive ? name : name.toLower();
}

// What no library's check should list: hidden files, those of Calibre,
// Cousoir and the sync services, and the litter of file managers.
bool ignoredName(const QString &name)
{
    return name.startsWith(u'.') || name.compare(QLatin1String("Thumbs.db"), Qt::CaseInsensitive) == 0
        || name.compare(QLatin1String("desktop.ini"), Qt::CaseInsensitive) == 0
        || name.endsWith(QLatin1String(".nextcloud")) || name.endsWith(QLatin1String(".owncloud"))
        || ConcurrencyGuard::isConflictCopy(name);
}

// IGNORE_AT_TOP_LEVEL of check_library.py, with SQLite's files beside them.
bool ignoredAtTop(const QString &name)
{
    static const QStringList files{ QStringLiteral("metadata.db"), QStringLiteral("metadata_db_prefs_backup.json"),
                                    QStringLiteral("metadata_pre_restore.db"), QStringLiteral("full-text-search.db") };
    for (const QString &file : files) {
        if (name == file || name == file + QStringLiteral("-journal") || name == file + QStringLiteral("-wal")
            || name == file + QStringLiteral("-shm"))
            return true;
    }
    return false;
}

QFileInfoList entries(const QString &folder)
{
    return QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System, QDir::Name);
}

LibraryCheck::OutsideFile outside(const QString &path, const QFileInfo &info, int book = 0)
{
    return { path, info.isDir(), book, info.isDir() ? 0 : info.size() };
}

// scan_library() and process_book(): author folders, the book folders in
// them, the files in those.
void scanFolder(const LibraryData &data, const LibraryCheck::Options &options, LibraryCheck::Findings *findings)
{
    static const QRegularExpression bookFolder(QStringLiteral("^.* \\((\\d+)\\)$"));
    QHash<QString, const Book *> books;
    for (const Book &book : data.books) {
        if (!book.path.isEmpty())
            books.insert(fileKey(book.path), &book);
    }
    const QDir library(data.libraryPath);
    for (const QFileInfo &top : entries(data.libraryPath)) {
        if (options.canceled && options.canceled())
            return;
        const QString author = top.fileName();
        if (ignoredName(author) || ignoredAtTop(author))
            continue;
        if (!top.isDir()) {
            findings->outside.append(outside(author, top));
            continue;
        }
        if (!top.isReadable()) {
            findings->unreadable.append(author);
            continue;
        }
        QList<LibraryCheck::OutsideFile> unknown;
        QList<QPair<QString, const Book *>> known;
        for (const QFileInfo &child : entries(top.filePath())) {
            if (ignoredName(child.fileName()))
                continue;
            const QString path = author + u'/' + child.fileName();
            const Book *book = books.value(fileKey(path));
            if (child.isDir() && book) {
                known.append({ path, book });
                continue;
            }
            const QRegularExpressionMatch match = bookFolder.match(child.fileName());
            unknown.append(outside(path, child, child.isDir() && match.hasMatch() ? match.captured(1).toInt() : 0));
        }
        // An author folder without a book of the library's: one entry.
        if (known.isEmpty()) {
            findings->outside.append(outside(author, top));
            continue;
        }
        findings->outside += unknown;
        for (const auto &[path, book] : std::as_const(known)) {
            const QFileInfo folder(library.filePath(path));
            if (!folder.isReadable()) {
                findings->unreadable.append(path);
                continue;
            }
            // NORMALS of check_library.py, and the book's own files.
            QSet<QString> expected{ QStringLiteral("metadata.opf"), QStringLiteral("cover.jpg"), QStringLiteral("data") };
            for (const BookFormat &format : book->formats) {
                if (!format.file.isEmpty())
                    expected.insert(fileKey(QFileInfo(format.file).fileName()));
            }
            for (const QFileInfo &file : entries(folder.filePath())) {
                if (!ignoredName(file.fileName()) && !expected.contains(fileKey(file.fileName())))
                    findings->outside.append(outside(path + u'/' + file.fileName(), file));
            }
        }
    }
    findings->scanned = true;
}

} // namespace

namespace LibraryCheck {

QStringList nameWords(const QString &name)
{
    QString folded = CalibreText::foldForSearch(name);
    static const QRegularExpression apostrophes(QStringLiteral("['’ʼ`]"));
    folded.remove(apostrophes);
    // Anything but a letter or a digit separates words, "Jean-Pierre" and
    // "J.R.R." included.
    for (qsizetype i = 0; i < folded.size(); ++i) {
        const QChar c = folded.at(i);
        if (c.isHighSurrogate() && i + 1 < folded.size() && folded.at(i + 1).isLowSurrogate()) {
            if (!QChar::isLetterOrNumber(QChar::surrogateToUcs4(c, folded.at(i + 1)))) {
                folded[i] = u' ';
                folded[i + 1] = u' ';
            }
            ++i;
        } else if (!c.isLetterOrNumber()) {
            folded[i] = u' ';
        }
    }
    return folded.split(u' ', Qt::SkipEmptyParts);
}

QString authorKey(const QString &name)
{
    QStringList words = nameWords(name);
    words.sort();
    return words.join(u' ');
}

bool sameByInitials(const QString &first, const QString &second, const CalibreText::Tweaks &tweaks)
{
    const std::optional<NameParts> a = nameParts(first, tweaks);
    const std::optional<NameParts> b = nameParts(second, tweaks);
    if (!a || !b || a->surname != b->surname || a->given.size() != b->given.size())
        return false;
    // A surname of initials says too little.
    if (std::any_of(a->surname.cbegin(), a->surname.cend(), [](const QString &word) { return word.size() < 2; }))
        return false;
    bool differ = false;
    for (qsizetype i = 0; i < a->given.size(); ++i) {
        const QString &x = a->given.at(i);
        const QString &y = b->given.at(i);
        if (x == y)
            continue;
        if (!(x.size() == 1 && y.startsWith(x)) && !(y.size() == 1 && x.startsWith(y)))
            return false;
        differ = true;
    }
    return differ;
}

QString isbnKey(const QString &value)
{
    const QString isbn = CalibreText::checkIsbn(value);
    if (isbn.size() != 10)
        return isbn;
    // Its ISBN-13: 978, the first nine digits, and their check digit.
    const QString digits = QStringLiteral("978") + isbn.left(9);
    int sum = 0;
    for (qsizetype i = 0; i < digits.size(); ++i)
        sum += digits.at(i).digitValue() * (i % 2 ? 3 : 1);
    return digits + QString::number((10 - sum % 10) % 10);
}

QString key(const DuplicateGroup &group)
{
    return QStringLiteral("duplicates:") + idList(group.books);
}

QString key(const AuthorGroup &group)
{
    return QStringLiteral("authors:") + idList(group.authors);
}

QString key(const MissingFiles &missing)
{
    return QStringLiteral("missing:%1:%2").arg(missing.book).arg(missing.formats.join(u','));
}

QString key(const OutsideFile &file)
{
    return QStringLiteral("outside:") + file.path;
}

QStringList ignoredKeys(const QString &uuid)
{
    QStringList keys;
    const QString prefix = uuid + u'|';
    for (const QString &entry : QSettings().value(QLatin1String(ignoredSetting)).toStringList()) {
        if (!uuid.isEmpty() && entry.startsWith(prefix))
            keys.append(entry.mid(prefix.size()));
    }
    return keys;
}

void ignore(const QString &uuid, const QString &key)
{
    if (uuid.isEmpty() || key.isEmpty() || ignoredKeys(uuid).contains(key))
        return;
    QSettings settings;
    settings.setValue(QLatin1String(ignoredSetting),
                      settings.value(QLatin1String(ignoredSetting)).toStringList() << uuid + u'|' + key);
}

void clearIgnored(const QString &uuid)
{
    QSettings settings;
    QStringList entries = settings.value(QLatin1String(ignoredSetting)).toStringList();
    const QString prefix = uuid + u'|';
    entries.removeIf([&prefix](const QString &entry) { return entry.startsWith(prefix); });
    settings.setValue(QLatin1String(ignoredSetting), entries);
}

Findings check(const LibraryData &data, const Options &options)
{
    Findings findings;
    const Authors authors = groupAuthors(data.authors, options.tweaks);
    findings.authorVariants = authors.groups;
    QHash<QString, QStringList> byLowerName;
    for (const FacetItem &author : data.authors)
        byLowerName[author.name.toLower()].append(author.name);
    for (const QStringList &names : std::as_const(byLowerName)) {
        if (names.size() > 1)
            findings.caseDuplicates += names;
    }
    findings.caseDuplicates.sort();
    findings.duplicates = groupBooks(data, authors, options);
    findings.missing = missingFiles(data);
    if (options.scanFiles && !data.libraryPath.isEmpty())
        scanFolder(data, options, &findings);
    return findings;
}

bool keepOne(const LibraryData &data, int keep, const QList<int> &others, CommitEngine::Request *request,
             QString *error)
{
    if (!data.book(keep))
        return fail(error, QObject::tr("This book is no longer in the library."));
    QList<int> trashed;
    for (const int other : others) {
        if (other == keep || trashed.contains(other))
            continue;
        if (!data.book(other))
            return fail(error, QObject::tr("This book is no longer in the library."));
        trashed.append(other);
    }
    if (trashed.isEmpty())
        return fail(error, QObject::tr("There is no other copy to move to the trash."));
    for (const int other : std::as_const(trashed))
        request->formatCopies.append({ other, keep });
    request->trash += trashed;
    return true;
}

QList<BookEdit> mergeAuthors(const LibraryData &data, const QList<int> &authorIds, int keepAuthorId, QString *error)
{
    QString kept;
    for (const FacetItem &author : data.authors) {
        if (author.id == keepAuthorId)
            kept = author.name;
    }
    if (kept.isEmpty() || !authorIds.contains(keepAuthorId)) {
        fail(error, QObject::tr("This author is no longer in the library."));
        return {};
    }
    QSet<int> variants(authorIds.cbegin(), authorIds.cend());
    variants.remove(keepAuthorId);
    QList<BookEdit> edits;
    for (const Book &book : data.books) {
        if (std::none_of(book.authorIds.cbegin(), book.authorIds.cend(), [&variants](int id) { return variants.contains(id); }))
            continue;
        // The kept name as the library writes it, in the variant's place:
        // nothing changes case.
        QStringList names;
        for (qsizetype i = 0; i < book.authorIds.size(); ++i) {
            const QString name = variants.contains(book.authorIds.at(i)) ? kept : book.authors.value(i);
            if (std::none_of(names.cbegin(), names.cend(),
                             [&name](const QString &other) { return other.compare(name, Qt::CaseInsensitive) == 0; }))
                names.append(name);
        }
        BookEdit edit;
        edit.bookId = book.id;
        edit.authors = names;
        if (!authorSortFollows(data, book))
            edit.authorSort = book.authorSort;
        edits.append(edit);
    }
    if (edits.isEmpty())
        fail(error, QObject::tr("No book is filed under these names any more."));
    return edits;
}

bool authorSortFollows(const LibraryData &data, const Book &book)
{
    QStringList sorts;
    for (const int id : book.authorIds) {
        for (const FacetItem &author : data.authors) {
            if (author.id == id)
                sorts.append(author.sort);
        }
    }
    return book.authorSort == sorts.join(QStringLiteral(" & "));
}

} // namespace LibraryCheck
