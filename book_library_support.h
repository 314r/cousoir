#pragma once

// What the parts of BookLibrary share: the sections, the columns, the books' files
// and covers, words as searches take them.

#include "book_library.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextDocument>
#include <QUrl>
#include <QPromise>
#include <QSet>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <numeric>

#include "book_import.h"
#include "book_text_index.h"
#include "new_library.h"
#include "book_text_search.h"
#ifdef COUSOIR_READER
#include "book_reader.h"
#endif
#include "calibre_db.h"
#include "calibre_text.h"
#include "concurrency_guard.h"
#include "conversion.h"
#include "cover_cache.h"
#include "field_writers.h"
#include "cousoir_paths.h"
#include "description.h"
#include "palette.h"
#include "sqlite_db.h"


namespace LibrarySupport {


inline const QStringList &sections()
{
    static const QStringList names = { QStringLiteral("books"), QStringLiteral("authors"), QStringLiteral("series"),
                                       QStringLiteral("tags"), QStringLiteral("languages"),
                                       QStringLiteral("publishers"), QStringLiteral("trash"),
                                       QStringLiteral("check"), QStringLiteral("highlights"),
                                       QStringLiteral("text"), QStringLiteral("reading") };
    return names;
}

inline bool isFacetSortOption(const QString &option)
{
    return option == QLatin1String("name") || option == QLatin1String("nameDesc")
        || option == QLatin1String("count") || option == QLatin1String("countDesc");
}

// The columns Cousoir shows a library Calibre has shown none of.
inline const QStringList defaultColumns{ QStringLiteral("title"), QStringLiteral("authors"), QStringLiteral("series"),
                                  QStringLiteral("formats"), QStringLiteral("timestamp"), QStringLiteral("rating") };

// The built-in columns of Calibre's list (column_map of
// gui2/library/models.py) and how wide their text is.
struct ColumnInfo
{
    const char *key;
    const char *title;
    const char *kind;
};
inline const ColumnInfo builtinColumns[] = {
    { "title", QT_TRANSLATE_NOOP("BookLibrary", "Title"), "text" },
    { "authors", QT_TRANSLATE_NOOP("BookLibrary", "Authors"), "text" },
    { "series", QT_TRANSLATE_NOOP("BookLibrary", "Series"), "text" },
    { "rating", QT_TRANSLATE_NOOP("BookLibrary", "Rating"), "rating" },
    { "tags", QT_TRANSLATE_NOOP("BookLibrary", "Tags"), "long" },
    { "publisher", QT_TRANSLATE_NOOP("BookLibrary", "Publisher"), "text" },
    { "pubdate", QT_TRANSLATE_NOOP("BookLibrary", "Published"), "date" },
    { "timestamp", QT_TRANSLATE_NOOP("BookLibrary", "Added"), "date" },
    { "last_modified", QT_TRANSLATE_NOOP("BookLibrary", "Modified"), "date" },
    { "size", QT_TRANSLATE_NOOP("BookLibrary", "Size"), "number" },
    { "languages", QT_TRANSLATE_NOOP("BookLibrary", "Languages"), "text" },
    { "formats", QT_TRANSLATE_NOOP("BookLibrary", "Formats"), "text" },
    { "identifiers", QT_TRANSLATE_NOOP("BookLibrary", "Identifiers"), "long" },
    { "pages", QT_TRANSLATE_NOOP("BookLibrary", "Pages"), "number" },
    // Cousoir's own, from where the book was read last.
    { "progress", QT_TRANSLATE_NOOP("BookLibrary", "Progress"), "number" },
    { "last_read", QT_TRANSLATE_NOOP("BookLibrary", "Last read"), "date" },
    { "id", QT_TRANSLATE_NOOP("BookLibrary", "Id"), "number" },
    { "path", QT_TRANSLATE_NOOP("BookLibrary", "Path"), "long" },
};

// The `columns` the list can show, by availableColumns(), each once, the
// title first.
inline QStringList knownColumns(const QStringList &columns, const QVariantList &available)
{
    QStringList known;
    for (const QVariant &column : available)
        known.append(column.toMap().value(QStringLiteral("key")).toString());
    QStringList kept{ QStringLiteral("title") };
    for (const QString &column : columns) {
        if (known.contains(column) && !kept.contains(column))
            kept.append(column);
    }
    return kept;
}

// How the books and items sort: as the system's language sorts them, case
// aside.
inline QCollator listCollator()
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

inline QString localDateTime(const QDateTime &date)
{
    return BookDisplay::dateTime(date);
}

// A book identifier's label and, where there is one, its page.
inline QVariantMap identifierMap(const QString &type, const QString &value)
{
    QString label = type;
    QString url;
    const QString key = type.toLower();
    if (key == QLatin1String("isbn")) {
        label = QStringLiteral("ISBN");
    } else if (key == QLatin1String("amazon") || key == QLatin1String("mobi-asin") || key == QLatin1String("asin")) {
        label = QStringLiteral("Amazon");
        url = QStringLiteral("https://www.amazon.com/dp/%1").arg(value);
    } else if (key == QLatin1String("google")) {
        label = QStringLiteral("Google Books");
        url = QStringLiteral("https://books.google.com/books?id=%1").arg(value);
    } else if (key == QLatin1String("goodreads")) {
        label = QStringLiteral("Goodreads");
        url = QStringLiteral("https://www.goodreads.com/book/show/%1").arg(value);
    } else if (key == QLatin1String("doi")) {
        label = QStringLiteral("DOI");
        url = QStringLiteral("https://doi.org/%1").arg(value);
    } else if (key == QLatin1String("uri") || key == QLatin1String("url")) {
        label = QStringLiteral("URL");
        url = value;
    }
    return {
        { QStringLiteral("type"), type },
        { QStringLiteral("label"), label },
        { QStringLiteral("value"), value },
        { QStringLiteral("url"), url },
    };
}

inline const QStringList &readerFormats()
{
    static const QStringList formats = { QStringLiteral("EPUB"), QStringLiteral("KEPUB"), QStringLiteral("AZW3"),
                                         QStringLiteral("MOBI"), QStringLiteral("PDF") };
    return formats;
}

// The words of a search, each to be found somewhere in a book.
inline QStringList searchWords(const QString &text)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    return CalibreText::foldForSearch(text).split(spaces, Qt::SkipEmptyParts);
}

inline bool matchesAll(const QString &haystack, const QStringList &words)
{
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &word) { return haystack.contains(word); });
}

// BOOK_EXTENSIONS of ebooks/__init__.py: the files of a folder Calibre adds.
inline const QSet<QString> &bookExtensions()
{
    static const QSet<QString> extensions = [] {
        QSet<QString> result;
        for (const char *extension :
             { "lrf", "rar", "zip", "rtf", "lit", "txt", "txtz", "text", "htm", "xhtm", "html", "htmlz", "xhtml", "pdf",
               "pdb", "updb", "pdr", "prc", "mobi", "azw", "doc", "epub", "fb2", "fbz", "djv", "djvu", "lrx", "cbr",
               "cb7", "cbz", "cbc", "oebzip", "rb", "imp", "odt", "chm", "tpz", "azw1", "pml", "pmlz", "mbp", "tan",
               "snb", "xps", "oxps", "azw4", "book", "zbf", "pobi", "docx", "docm", "md", "textile", "markdown",
               "ibook", "ibooks", "iba", "azw3", "ps", "kepub", "kfx", "kpf" })
            result.insert(QString::fromLatin1(extension));
        return result;
    }();
    return extensions;
}

inline QString localPath(const QVariant &url)
{
    const QUrl parsed = url.toUrl();
    if (parsed.isLocalFile())
        return parsed.toLocalFile();
    const QString text = url.toString();
    return text.startsWith(QLatin1String("file:")) ? QUrl(text).toLocalFile() : text;
}

// The files to add: those given, and the book files of the folders given.
inline QStringList bookFiles(const QVariantList &urls)
{
    QStringList files;
    for (const QVariant &url : urls) {
        const QString path = localPath(url);
        const QFileInfo info(path);
        if (info.isFile()) {
            files.append(info.absoluteFilePath());
        } else if (info.isDir()) {
            QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            QStringList found;
            while (it.hasNext()) {
                const QFileInfo file(it.next());
                if (!file.fileName().startsWith(u'.') && bookExtensions().contains(file.suffix().toLower()))
                    found.append(file.absoluteFilePath());
            }
            found.sort();
            files += found;
        }
    }
    files.removeDuplicates();
    return files;
}

// The book's cover.jpg as it is, empty when it has none.
inline QByteArray coverBytes(const Book &book)
{
    QFile file(book.coverFile);
    return !book.coverFile.isEmpty() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The key of the library's covers in CoverCache: its cache folder's name.
inline QString coverKeyOf(const QString &libraryPath)
{
    return QFileInfo(CousoirPaths::cacheFolder(libraryPath)).fileName();
}


} // namespace LibrarySupport
