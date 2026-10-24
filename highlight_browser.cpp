#include "highlight_browser.h"

#include <QClipboard>
#include <QCollator>
#include <QGuiApplication>
#include <QScopeGuard>

#include "book.h"
#include "book_library.h"
#include "highlight_sync.h"
#include "sqlite_db.h"

using PythonJson::Value;

namespace {

// kbextract2's monogram(): the title's first letter or digit, upper-cased.
QString monogramOf(const QString &title)
{
    for (const QChar c : title) {
        if (c.isLetterOrNumber())
            return QString(c).toUpper();
    }
    return QStringLiteral("·");
}

// kbextract2's bookTile(): FNV-1a of the title's bytes picks one of its six
// colours, so a book keeps its own.
int tintOf(const QString &title)
{
    quint32 hash = 2166136261u;
    for (const char byte : title.toUtf8()) {
        hash ^= quint8(byte);
        hash *= 16777619u;
    }
    return int(hash % 6);
}

} // namespace

HighlightBrowser::HighlightBrowser(QObject *parent)
    : QObject(parent)
{
}

BookLibrary *HighlightBrowser::library() const
{
    return m_library;
}

HighlightSync *HighlightBrowser::sync() const
{
    return m_sync;
}

void HighlightBrowser::setSync(HighlightSync *sync)
{
    if (m_sync == sync)
        return;
    if (m_sync)
        disconnect(m_sync, nullptr, this, nullptr);
    m_sync = sync;
    // What the Kobos have that the library cannot hold, as it changes.
    if (m_sync) {
        connect(m_sync, &HighlightSync::leftChanged, this, &HighlightBrowser::refresh);
        connect(m_sync, &HighlightSync::sourcesChanged, this, &HighlightBrowser::refresh);
    }
    emit syncChanged();
    refresh();
}

void HighlightBrowser::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    // Read again with the library: a highlight made since is there.
    if (m_library)
        connect(m_library, &BookLibrary::libraryChanged, this, &HighlightBrowser::refresh);
    emit libraryChanged();
    refresh();
}

QVariantList HighlightBrowser::books() const
{
    return m_books;
}

QString HighlightBrowser::current() const
{
    return m_current;
}

void HighlightBrowser::setCurrent(const QString &current)
{
    if (m_current == current)
        return;
    m_current = current;
    emit currentChanged();
    buildDocument();
}

QVariantMap HighlightBrowser::document() const
{
    return m_document;
}

int HighlightBrowser::count() const
{
    return m_count;
}

QString HighlightBrowser::error() const
{
    return m_error;
}

void HighlightBrowser::refresh()
{
    m_groups.clear();
    m_books.clear();
    m_count = 0;
    m_error.clear();
    const auto done = qScopeGuard([this] {
        emit booksChanged();
        // The book shown, while it still has highlights.
        if (!m_current.isEmpty() && !groupOf(m_current)) {
            m_current.clear();
            emit currentChanged();
        }
        buildDocument();
    });
    if (!m_library || m_library->libraryPath().isEmpty() || !m_library->loaded())
        return;
    // The copy of the database the library was read from.
    SqliteDb db;
    QString error;
    if (!db.open(BookLibrary::cacheFolder(m_library->libraryPath()) + QStringLiteral("/snapshot.db"),
                 SqliteDb::Mode::Immutable, &error)) {
        m_error = error;
        return;
    }
    AnnotationSearch::Query query;
    query.type = QStringLiteral("highlight");
    query.all = true;
    const QList<AnnotationSearch::Hit> hits = AnnotationSearch::find(db, query, &error);
    if (!error.isEmpty()) {
        m_error = error;
        return;
    }
    const LibraryData &data = m_library->data();
    // Only the library's books: a book gone keeps no highlights to show.
    QList<AnnotationSearch::Hit> kept;
    for (const AnnotationSearch::Hit &hit : hits) {
        if (data.book(hit.bookId))
            kept.append(hit);
    }
    QHash<int, qsizetype> groupOf;
    for (const auto &[bookId, group] : AnnotationSearch::groupedByBook(kept, data)) {
        const Book *book = data.book(bookId);
        groupOf.insert(bookId, m_groups.size());
        m_groups.append({ QStringLiteral("book:%1").arg(bookId), bookId, book->title, book->authors, group, {}, {} });
    }

    // What the Kobos have that the library cannot hold: with their book when
    // the library has it, after the library's books otherwise, by Kobo.
    const QHash<QString, QList<KoboSync::Left>> left = m_sync ? m_sync->left() : QHash<QString, QList<KoboSync::Left>>();
    const auto nameOf = [this](const QString &device) { return m_sync ? m_sync->nameOf(device) : device; };
    QStringList devices = left.keys();
    std::sort(devices.begin(), devices.end(),
              [&nameOf](const QString &a, const QString &b) { return nameOf(a).localeAwareCompare(nameOf(b)) < 0; });
    QList<Group> koboOnly;
    for (const QString &device : std::as_const(devices)) {
        for (const KoboSync::Left &book : left.value(device)) {
            QList<KoboHighlights::Highlight> shown;
            for (const KoboHighlights::Highlight &highlight : book.book.highlights) {
                if (!highlight.hidden)
                    shown.append(highlight);
            }
            if (shown.isEmpty())
                continue;
            const Book *libraryBook = book.bookId > 0 ? data.book(book.bookId) : nullptr;
            if (libraryBook && groupOf.contains(libraryBook->id)) {
                m_groups[groupOf.value(libraryBook->id)].left += shown;
            } else if (libraryBook) {
                groupOf.insert(libraryBook->id, m_groups.size());
                m_groups.append({ QStringLiteral("book:%1").arg(libraryBook->id), libraryBook->id, libraryBook->title,
                                  libraryBook->authors, {}, shown, {} });
            } else {
                koboOnly.append({ QStringLiteral("kobo:%1:%2").arg(device, book.book.volumeId), 0, book.book.title,
                                  book.book.authors, {}, shown, tr("On %1, not in the library").arg(nameOf(device)) });
            }
        }
    }
    // The library's books by title, as AnnotationSearch groups them, those
    // with only highlights left out of it among them.
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const auto sortTitle = [&data](const Group &group) {
        const Book *book = data.book(group.bookId);
        return book && !book->sort.isEmpty() ? book->sort : group.title;
    };
    std::stable_sort(m_groups.begin(), m_groups.end(), [&](const Group &a, const Group &b) {
        return collator.compare(sortTitle(a), sortTitle(b)) < 0;
    });
    m_groups += koboOnly;
    // A book whose highlights are all blank has none to show.
    m_groups.removeIf([this](const Group &group) { return HighlightFormats::chapters(entriesOf(group)).isEmpty(); });

    for (const Group &group : std::as_const(m_groups)) {
        // As kbextract2 counts them: a highlight with a note is a note.
        int highlights = 0;
        int notes = 0;
        for (const HighlightFormats::Entry &entry : entriesOf(group)) {
            if (!entry.note.isEmpty())
                ++notes;
            else if (!entry.highlight.isEmpty())
                ++highlights;
        }
        m_count += highlights + notes;
        m_books.append(QVariantMap{
            { QStringLiteral("key"), group.key },
            { QStringLiteral("bookId"), group.bookId },
            { QStringLiteral("title"), group.title },
            { QStringLiteral("authors"), group.authors.join(QLatin1String(" & ")) },
            { QStringLiteral("counts"), (highlights == 1 ? tr("%1 highlight") : tr("%1 highlights")).arg(highlights)
                                            + QStringLiteral(" · ")
                                            + (notes == 1 ? tr("%1 note") : tr("%1 notes")).arg(notes) },
            { QStringLiteral("cover"), group.bookId > 0 ? m_library->coverUrl(group.bookId) : QString() },
            { QStringLiteral("monogram"), monogramOf(group.title) },
            { QStringLiteral("tint"), tintOf(group.title) },
            { QStringLiteral("section"), group.section },
        });
    }
}

void HighlightBrowser::buildDocument()
{
    m_document.clear();
    if (const Group *group = groupOf(m_current)) {
        QVariantList chapters;
        for (const HighlightFormats::Chapter &chapter : HighlightFormats::chapters(entriesOf(*group))) {
            QVariantList entries;
            for (const HighlightFormats::Entry &entry : chapter.entries) {
                const QString text = HighlightFormats::reflowedHighlight(entry.highlight);
                entries.append(QVariantMap{
                    { QStringLiteral("paragraphs"), text.isEmpty() ? QStringList() : text.split(QStringLiteral("\n\n")) },
                    { QStringLiteral("note"), HighlightFormats::normalizedText(entry.note) },
                });
            }
            chapters.append(QVariantMap{
                { QStringLiteral("title"), chapter.title.isEmpty() ? tr("Untitled chapter") : chapter.title },
                { QStringLiteral("entries"), entries },
            });
        }
        const QVariantMap book = m_books.value(int(group - m_groups.constData())).toMap();
        m_document = QVariantMap{
            { QStringLiteral("title"), group->title },
            { QStringLiteral("authors"), book.value(QStringLiteral("authors")) },
            { QStringLiteral("counts"), book.value(QStringLiteral("counts")) },
            { QStringLiteral("chapters"), chapters },
        };
    }
    emit documentChanged();
}

const HighlightBrowser::Group *HighlightBrowser::groupOf(const QString &key) const
{
    for (const Group &group : m_groups) {
        if (group.key == key)
            return &group;
    }
    return nullptr;
}

QList<HighlightFormats::Entry> HighlightBrowser::entriesOf(const Group &group) const
{
    QList<HighlightFormats::Entry> entries;
    for (const AnnotationSearch::Hit &hit : group.hits) {
        const Value &annotation = hit.annotation;
        QStringList chapter;
        if (const Value *titles = annotation.find(QStringLiteral("toc_family_titles"))) {
            for (const Value &title : titles->items)
                chapter.append(title.string);
        }
        const Value *spine = annotation.find(QStringLiteral("spine_index"));
        // Without a chapter, those of a file go together.
        const QString key = !chapter.isEmpty() ? chapter.join(QChar(0x1f))
            : spine                            ? QStringLiteral("spine:%1").arg(spine->integer)
                                               : QString();
        entries.append({ annotation.text(QStringLiteral("uuid")), annotation.text(QStringLiteral("highlighted_text")),
                         annotation.text(QStringLiteral("notes")), key, chapter.isEmpty() ? QString() : chapter.last(),
                         spine ? std::optional<qint64>(spine->integer) : std::nullopt });
    }
    for (const KoboHighlights::Highlight &highlight : group.left)
        entries.append(KoboHighlights::entry(highlight));
    return entries;
}

QString HighlightBrowser::formatted(const QString &format, const QString &key) const
{
    const Group *group = groupOf(key.isEmpty() ? m_current : key);
    return group ? HighlightFormats::render(entriesOf(*group), HighlightFormats::formatNamed(format)) : QString();
}

void HighlightBrowser::copy(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}
