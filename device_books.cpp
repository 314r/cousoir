// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/gui2/device.py: set_books_in_library()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "device_books.h"

#include <QHash>
#include <QSet>

#include "book.h"
#include "calibre_text.h"

namespace {

// clean_string() of set_books_in_library(): lower case, letters and digits
// only, as Python's \W and _ leave them.
QString clean(const QString &text)
{
    QString cleaned;
    const QString lower = text.toLower();
    cleaned.reserve(lower.size());
    for (const QChar c : lower) {
        if (c.isLetterOrNumber())
            cleaned.append(c);
    }
    return cleaned;
}

} // namespace

namespace DeviceBooks {

void match(QList<DeviceBook> *books, const LibraryData &data)
{
    struct Titled
    {
        QHash<QString, int> authors;
        QHash<QString, int> authorSorts;
        QSet<int> ids;
    };
    QHash<QString, Titled> byTitle;
    QHash<QString, int> byUuid;
    // When two books share a title and authors, the last one wins: they are
    // the same as far as anyone can tell.
    for (const Book &book : data.books) {
        Titled &titled = byTitle[clean(book.title)];
        const QString authors = clean(CalibreText::authorsToString(book.authors));
        if (!authors.isEmpty())
            titled.authors.insert(authors, book.id);
        if (!book.authorSort.isEmpty())
            titled.authorSorts.insert(clean(book.authorSort), book.id);
        titled.ids.insert(book.id);
        byUuid.insert(book.uuid, book.id);
    }
    for (DeviceBook &book : *books) {
        book.bookId = 0;
        if (!book.uuid.isEmpty() && byUuid.contains(book.uuid)) {
            book.bookId = byUuid.value(book.uuid);
            continue;
        }
        const auto titled = byTitle.constFind(clean(book.title));
        if (titled == byTitle.cend())
            continue;
        if (book.applicationId && titled->ids.contains(book.applicationId)) {
            book.bookId = book.applicationId;
            continue;
        }
        const auto byAuthor = [&](const QString &key) {
            const int id = titled->authors.value(key, titled->authorSorts.value(key));
            if (id)
                book.bookId = id;
            return id != 0;
        };
        if (!book.authors.isEmpty() && !byAuthor(clean(CalibreText::authorsToString(book.authors)))) {
            for (const QString &author : std::as_const(book.authors)) {
                if (byAuthor(clean(author)))
                    break;
            }
        }
    }
}

QString sendFormat(const QStringList &formats, const QStringList &formatMap)
{
    for (const QString &format : formatMap) {
        if (formats.contains(format.toUpper()))
            return format.toUpper();
    }
    return {};
}

// The text of each element of the JSON array `json`, as written there.
QList<QByteArray> arrayElements(const QByteArray &json)
{
    QList<QByteArray> elements;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    qsizetype start = -1;
    for (qsizetype i = 0; i < json.size(); ++i) {
        const char c = json.at(i);
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (depth == 1 && start < 0 && !QChar::isSpace(uchar(c)) && c != ',' && c != ']')
            start = i;
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            if (--depth == 0 && start >= 0)
                elements.append(json.mid(start, i - start).trimmed());
        } else if (c == ',' && depth == 1 && start >= 0) {
            elements.append(json.mid(start, i - start).trimmed());
            start = -1;
        }
    }
    return elements;
}

QByteArray joinElements(const QList<QByteArray> &elements)
{
    return elements.isEmpty() ? QByteArray("[]") : "[\n  " + elements.join(",\n  ") + "\n]";
}

} // namespace DeviceBooks
