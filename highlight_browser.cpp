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
    AnnotationSearch::Query query;
    query.text = m_query;
    query.stemming = m_stemming;
    query.type = m_type;
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
    m_groups = AnnotationSearch::groupedByBook(kept, data);
    int index = 0;
    for (const auto &[bookId, group] : std::as_const(m_groups)) {
        const Book *book = data.book(bookId);
        m_rows.append(QVariantMap{
            { QStringLiteral("kind"), QStringLiteral("book") },
            { QStringLiteral("bookId"), bookId },
            { QStringLiteral("title"), book->title },
            { QStringLiteral("authors"), book->authors.join(QLatin1String(" & ")) },
            { QStringLiteral("count"), int(group.size()) },
        });
        for (const AnnotationSearch::Hit &hit : group) {
            const Value &annotation = hit.annotation;
            QStringList chapter;
            if (const Value *titles = annotation.find(QStringLiteral("toc_family_titles"))) {
                for (const Value &title : titles->items)
                    chapter.append(title.string);
            }
            const Value *style = annotation.find(QStringLiteral("style"));
            const Value *spine = annotation.find(QStringLiteral("spine_index"));
            m_rows.append(QVariantMap{
                { QStringLiteral("kind"), QStringLiteral("annotation") },
                { QStringLiteral("index"), index++ },
                { QStringLiteral("bookId"), bookId },
                { QStringLiteral("format"), hit.format },
                { QStringLiteral("type"), annotation.text(QStringLiteral("type")) },
                { QStringLiteral("html"), markedHtml(hit.text) },
                { QStringLiteral("notes"), annotation.text(QStringLiteral("notes")) },
                { QStringLiteral("chapter"), chapter.join(QStringLiteral(" › ")) },
                { QStringLiteral("date"), shortDate(annotation.text(QStringLiteral("timestamp"))) },
                { QStringLiteral("color"), style ? style->text(QStringLiteral("which")) : QString() },
                { QStringLiteral("spineIndex"), spine ? int(spine->integer) : -1 },
                { QStringLiteral("cfi"), annotation.text(QStringLiteral("start_cfi")) },
            });
        }
        m_count += int(group.size());
    }
}

QString HighlightBrowser::exportText(bool markdown, int bookId) const
{
    if (!m_library)
        return {};
    QList<AnnotationSearch::Hit> hits;
    for (const auto &[id, group] : m_groups) {
        if (bookId == 0 || id == bookId)
            hits += group;
    }
    return AnnotationSearch::exportText(hits, m_library->data(), markdown, m_library->interfaceLanguage());
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
