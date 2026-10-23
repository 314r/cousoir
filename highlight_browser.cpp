#include "highlight_browser.h"

#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QSaveFile>

#include "book.h"
#include "book_library.h"
#include "calibre_fts.h"
#include "highlight_sync.h"
#include "kobo_placement.h"
#include "sqlite_db.h"

using PythonJson::Value;

namespace {

// AnnotsResultsDelegate.result_data(): the snippet's parts around the words
// found, as HTML with those in bold.
QString markedHtml(QString text)
{
    text.replace(u'\x1f', u' ');
    const QStringList parts = text.split(AnnotationSearch::markStart);
    if (parts.size() < 2)
        return text.toHtmlEscaped();
    // Before, the words found, and the rest, its marks gone.
    const QString after = parts.mid(2).join(QString());
    return parts.at(0).toHtmlEscaped() + QStringLiteral("<b>") + parts.at(1).toHtmlEscaped() + QStringLiteral("</b>")
        + after.toHtmlEscaped();
}

QString shortDate(const QString &timestamp)
{
    const QDateTime date = QDateTime::fromString(timestamp, Qt::ISODateWithMs).toLocalTime();
    return date.isValid() ? QLocale::system().toString(date, QLocale::ShortFormat) : QString();
}

// Case and accents aside, as a search is typed.
QString searchable(const QString &text)
{
    QString folded;
    for (const QChar c : text.normalized(QString::NormalizationForm_D)) {
        if (c.category() != QChar::Mark_NonSpacing)
            folded += c.toLower();
    }
    return folded;
}

} // namespace

HighlightBrowser::HighlightBrowser(QObject *parent)
    : QObject(parent)
{
    // A search waits for the typing to pause.
    m_pause.setSingleShot(true);
    m_pause.setInterval(150);
    connect(&m_pause, &QTimer::timeout, this, &HighlightBrowser::search);
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

QString HighlightBrowser::query() const
{
    return m_query;
}

void HighlightBrowser::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    emit queryChanged();
    m_pause.start();
}

bool HighlightBrowser::stemming() const
{
    return m_stemming;
}

void HighlightBrowser::setStemming(bool stemming)
{
    if (m_stemming == stemming)
        return;
    m_stemming = stemming;
    emit queryChanged();
    refresh();
}

QString HighlightBrowser::type() const
{
    return m_type;
}

void HighlightBrowser::setType(const QString &type)
{
    if (m_type == type)
        return;
    m_type = type;
    emit queryChanged();
    refresh();
}

QString HighlightBrowser::source() const
{
    return m_source;
}

void HighlightBrowser::setSource(const QString &source)
{
    if (m_source == source)
        return;
    m_source = source;
    emit queryChanged();
    refresh();
}

QVariantList HighlightBrowser::sources() const
{
    return m_sources;
}

QVariantList HighlightBrowser::rows() const
{
    return m_rows;
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
    m_pause.stop();
    search();
}

void HighlightBrowser::search()
{
    m_groups.clear();
    m_rows.clear();
    m_sources.clear();
    m_count = 0;
    m_error.clear();
    const auto done = qScopeGuard([this] { emit rowsChanged(); });
    if (!m_library || m_library->libraryPath().isEmpty() || !m_library->loaded())
        return;
    // The copy of the database the library was read from, its search
    // index read with Calibre's tokenizer.
    SqliteDb db;
    QString error;
    if (!db.open(BookLibrary::cacheFolder(m_library->libraryPath()) + QStringLiteral("/snapshot.db"),
                 SqliteDb::Mode::Immutable, &error)
        || !CalibreFts::registerTokenizers(db.handle(), m_library->interfaceLanguage(), &error)) {
        m_error = error;
        return;
    }
    const QHash<QString, QList<KoboSync::Left>> left = m_sync ? m_sync->left() : QHash<QString, QList<KoboSync::Left>>();
    m_sources.append(QVariantMap{ { QStringLiteral("key"), QString() }, { QStringLiteral("name"), tr("All sources") } });
    // The viewer's first, then the others by name.
    QList<QPair<QString, QString>> users = AnnotationSearch::users(db);
    for (auto it = left.cbegin(); it != left.cend(); ++it) {
        if (!it.value().isEmpty() && !users.contains({ KoboSync::userType, it.key() }))
            users.append({ KoboSync::userType, it.key() });
    }
    std::sort(users.begin(), users.end(), [this](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
        const bool viewerA = a == QPair<QString, QString>(QStringLiteral("local"), QStringLiteral("viewer"));
        const bool viewerB = b == QPair<QString, QString>(QStringLiteral("local"), QStringLiteral("viewer"));
        if (viewerA != viewerB)
            return viewerA;
        return sourceName(a.first, a.second).localeAwareCompare(sourceName(b.first, b.second)) < 0;
    });
    for (const auto &[userType, user] : std::as_const(users))
        m_sources.append(QVariantMap{ { QStringLiteral("key"), userType + u':' + user },
                                      { QStringLiteral("name"), sourceName(userType, user) } });

    AnnotationSearch::Query query;
    query.text = m_query;
    query.stemming = m_stemming;
    query.type = m_type;
    if (!m_source.isEmpty()) {
        query.userType = m_source.section(u':', 0, 0);
        query.user = m_source.section(u':', 1);
    }
    const LibraryData &data = m_library->data();
    const QList<AnnotationSearch::Hit> hits = AnnotationSearch::find(db, query, &error);
    if (!error.isEmpty()) {
        m_error = tr("The search cannot be read: %1").arg(error);
        return;
    }
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
        m_groups.append({ QStringLiteral("book:%1").arg(bookId), bookId, book->title, book->authors, group, {} });
    }

    // What the Kobos have that the library cannot hold: with their book when
    // the library has it, after the library's books otherwise, by Kobo.
    QList<Group> added;
    QList<QPair<QString, QList<Group>>> koboOnly;
    QStringList devices = left.keys();
    std::sort(devices.begin(), devices.end(),
              [this](const QString &a, const QString &b) { return sourceName(KoboSync::userType, a).localeAwareCompare(
                                                                   sourceName(KoboSync::userType, b)) < 0; });
    for (const QString &device : std::as_const(devices)) {
        QList<Group> groups;
        for (const KoboSync::Left &book : left.value(device)) {
            QList<Left> shown;
            for (const KoboHighlights::Highlight &highlight : book.book.highlights) {
                if (shows(device, highlight))
                    shown.append({ device, highlight, book.reasons.value(highlight.id) });
            }
            if (shown.isEmpty())
                continue;
            const Book *libraryBook = book.bookId > 0 ? data.book(book.bookId) : nullptr;
            if (libraryBook && groupOf.contains(libraryBook->id)) {
                m_groups[groupOf.value(libraryBook->id)].left += shown;
            } else if (libraryBook) {
                groupOf.insert(libraryBook->id, -1);
                added.append({ QStringLiteral("book:%1").arg(libraryBook->id), libraryBook->id, libraryBook->title,
                               libraryBook->authors, {}, shown });
            } else {
                for (Left &entry : shown)
                    entry.reason.clear();
                groups.append({ QStringLiteral("kobo:%1:%2").arg(device, book.book.volumeId), 0, book.book.title,
                                book.book.authors, {}, shown });
            }
        }
        if (!groups.isEmpty())
            koboOnly.append({ device, groups });
    }
    // A library book with only highlights left out of it: after those with
    // some found, by title.
    std::stable_sort(added.begin(), added.end(),
                     [](const Group &a, const Group &b) { return a.title.localeAwareCompare(b.title) < 0; });
    m_groups += added;

    int index = 0;
    for (const Group &group : std::as_const(m_groups))
        appendGroup(group, &index);
    for (const auto &[device, groups] : std::as_const(koboOnly)) {
        m_rows.append(QVariantMap{ { QStringLiteral("kind"), QStringLiteral("heading") },
                                   { QStringLiteral("title"), tr("On %1, not in the library").arg(sourceName(KoboSync::userType, device)) } });
        for (const Group &group : groups) {
            m_groups.append(group);
            appendGroup(group, &index);
        }
    }
}

bool HighlightBrowser::shows(const QString &device, const KoboHighlights::Highlight &highlight) const
{
    if (highlight.hidden || m_type == QLatin1String("bookmark"))
        return false;
    if (!m_source.isEmpty() && m_source != KoboSync::userType + u':' + device)
        return false;
    if (m_query.trimmed().isEmpty())
        return true;
    // Not in Calibre's index: the query's words, each somewhere.
    const QString text = searchable(highlight.text + u' ' + highlight.note);
    for (QString word : m_query.split(u' ', Qt::SkipEmptyParts)) {
        word.remove(u'"');
        word.remove(u'*');
        if (word == QLatin1String("AND") || word == QLatin1String("OR") || word == QLatin1String("NOT") || word.isEmpty())
            continue;
        if (!text.contains(searchable(word)))
            return false;
    }
    return true;
}

QString HighlightBrowser::sourceName(const QString &userType, const QString &user) const
{
    if (userType == QLatin1String("local") && user == QLatin1String("viewer"))
        return tr("Cousoir and Calibre's viewer");
    if (userType == KoboSync::userType)
        return m_sync ? m_sync->nameOf(user) : user;
    if (userType == QLatin1String("web") && user == QLatin1String("*"))
        return tr("Calibre's Content server");
    return user;
}

void HighlightBrowser::appendGroup(const Group &group, int *index)
{
    int highlights = 0;
    int notes = 0;
    int bookmarks = 0;
    for (const AnnotationSearch::Hit &hit : group.hits) {
        if (hit.annotation.text(QStringLiteral("type")) == QLatin1String("bookmark"))
            ++bookmarks;
        else if (!hit.annotation.text(QStringLiteral("notes")).isEmpty())
            ++notes;
        else
            ++highlights;
    }
    for (const Left &entry : group.left) {
        if (!entry.highlight.note.isEmpty())
            ++notes;
        else
            ++highlights;
    }
    // As kbextract2 counts them: a highlight with a note is a note.
    QStringList counts;
    if (highlights > 0)
        counts.append((highlights == 1 ? tr("%1 highlight") : tr("%1 highlights")).arg(highlights));
    if (notes > 0)
        counts.append((notes == 1 ? tr("%1 note") : tr("%1 notes")).arg(notes));
    if (bookmarks > 0)
        counts.append((bookmarks == 1 ? tr("%1 bookmark") : tr("%1 bookmarks")).arg(bookmarks));
    m_rows.append(QVariantMap{
        { QStringLiteral("kind"), QStringLiteral("book") },
        { QStringLiteral("key"), group.key },
        { QStringLiteral("bookId"), group.bookId },
        { QStringLiteral("title"), group.title },
        { QStringLiteral("authors"), group.authors.join(QLatin1String(" & ")) },
        { QStringLiteral("count"), int(group.hits.size() + group.left.size()) },
        { QStringLiteral("counts"), counts.join(QStringLiteral(" · ")) },
    });
    QString previous;
    const auto chapterChange = [&previous](const QString &chapter) {
        const QString shown = chapter != previous ? chapter : QString();
        previous = chapter;
        return shown;
    };
    for (const AnnotationSearch::Hit &hit : group.hits) {
        const Value &annotation = hit.annotation;
        QStringList chapter;
        if (const Value *titles = annotation.find(QStringLiteral("toc_family_titles"))) {
            for (const Value &title : titles->items)
                chapter.append(title.string);
        }
        const Value *style = annotation.find(QStringLiteral("style"));
        const Value *spine = annotation.find(QStringLiteral("spine_index"));
        const QString chapterText = chapter.join(QStringLiteral(" › "));
        m_rows.append(QVariantMap{
            { QStringLiteral("kind"), QStringLiteral("annotation") },
            { QStringLiteral("index"), (*index)++ },
            { QStringLiteral("key"), group.key },
            { QStringLiteral("bookId"), group.bookId },
            { QStringLiteral("format"), hit.format },
            { QStringLiteral("type"), annotation.text(QStringLiteral("type")) },
            { QStringLiteral("html"), markedHtml(hit.text) },
            { QStringLiteral("notes"), annotation.text(QStringLiteral("notes")) },
            { QStringLiteral("chapter"), chapterText },
            { QStringLiteral("newChapter"), chapterChange(chapterText) },
            { QStringLiteral("date"), shortDate(annotation.text(QStringLiteral("timestamp"))) },
            { QStringLiteral("color"), style ? style->text(QStringLiteral("which")) : QString() },
            { QStringLiteral("spineIndex"), spine ? int(spine->integer) : -1 },
            { QStringLiteral("cfi"), annotation.text(QStringLiteral("start_cfi")) },
            { QStringLiteral("source"), hit.userType + u':' + hit.user },
            { QStringLiteral("sourceName"), sourceName(hit.userType, hit.user) },
            { QStringLiteral("placed"), true },
            { QStringLiteral("reason"), QString() },
        });
        ++m_count;
    }
    for (const Left &entry : group.left) {
        const KoboHighlights::Highlight &highlight = entry.highlight;
        const QDateTime created = KoboPlacement::changedAt(highlight);
        QString reason;
        if (entry.reason == QLatin1String("note without highlighted text"))
            reason = tr("A note on no text");
        else if (entry.reason == QLatin1String("no EPUB in the library"))
            reason = tr("The library has no EPUB of it");
        else if (entry.reason == QLatin1String("book not readable"))
            reason = tr("The library's copy cannot be read");
        else if (!entry.reason.isEmpty())
            reason = tr("Not found in the library's copy");
        m_rows.append(QVariantMap{
            { QStringLiteral("kind"), QStringLiteral("annotation") },
            { QStringLiteral("index"), (*index)++ },
            { QStringLiteral("key"), group.key },
            { QStringLiteral("bookId"), group.bookId },
            { QStringLiteral("format"), QString() },
            { QStringLiteral("type"), QStringLiteral("highlight") },
            { QStringLiteral("html"), HighlightFormats::reflowedHighlight(highlight.text).toHtmlEscaped() },
            { QStringLiteral("notes"), highlight.note },
            { QStringLiteral("chapter"), highlight.chapterTitle },
            { QStringLiteral("newChapter"), chapterChange(highlight.chapterTitle) },
            { QStringLiteral("date"), created.isValid() ? QLocale::system().toString(created.toLocalTime(), QLocale::ShortFormat) : QString() },
            { QStringLiteral("color"), KoboHighlights::calibreColor(highlight.color) },
            { QStringLiteral("spineIndex"), -1 },
            { QStringLiteral("cfi"), QString() },
            { QStringLiteral("source"), KoboSync::userType + u':' + entry.device },
            { QStringLiteral("sourceName"), sourceName(KoboSync::userType, entry.device) },
            { QStringLiteral("placed"), false },
            { QStringLiteral("reason"), reason },
        });
        ++m_count;
    }
}

QString HighlightBrowser::exportText(bool markdown, int bookId) const
{
    if (!m_library)
        return {};
    QList<AnnotationSearch::Hit> hits;
    for (const Group &group : m_groups) {
        if (bookId == 0 || group.bookId == bookId)
            hits += group.hits;
    }
    return AnnotationSearch::exportText(hits, m_library->data(), markdown, m_library->interfaceLanguage());
}

QString HighlightBrowser::formatted(const QString &format, const QString &key) const
{
    QList<HighlightFormats::Book> books;
    for (const Group &group : m_groups) {
        if (!key.isEmpty() && group.key != key)
            continue;
        HighlightFormats::Book book{ group.title, {} };
        for (const AnnotationSearch::Hit &hit : group.hits) {
            const Value &annotation = hit.annotation;
            if (annotation.text(QStringLiteral("type")) != QLatin1String("highlight"))
                continue;
            QStringList chapter;
            if (const Value *titles = annotation.find(QStringLiteral("toc_family_titles"))) {
                for (const Value &title : titles->items)
                    chapter.append(title.string);
            }
            const Value *spine = annotation.find(QStringLiteral("spine_index"));
            book.entries.append({ annotation.text(QStringLiteral("uuid")), annotation.text(QStringLiteral("highlighted_text")),
                                  annotation.text(QStringLiteral("notes")), chapter.join(QChar(0x1f)),
                                  chapter.isEmpty() ? QString() : chapter.last(),
                                  spine ? std::optional<qint64>(spine->integer) : std::nullopt });
        }
        for (const Left &entry : group.left)
            book.entries.append(KoboHighlights::entry(entry.highlight));
        books.append(book);
    }
    const HighlightFormats::Format kind = HighlightFormats::formatNamed(format);
    // One book as kbextract2 writes it, without its title.
    return !key.isEmpty() && books.size() == 1 ? HighlightFormats::render(books.first().entries, kind)
                                               : HighlightFormats::render(books, kind);
}

void HighlightBrowser::copy(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

QString HighlightBrowser::save(const QString &text, const QUrl &file) const
{
    // Calibre's export writes a byte order mark first.
    QSaveFile out(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!out.open(QIODevice::WriteOnly) || out.write("\xef\xbb\xbf") < 0 || out.write(text.toUtf8()) < 0 || !out.commit())
        return tr("Cannot write %1: %2").arg(QFileInfo(out.fileName()).fileName(), out.errorString());
    return {};
}
