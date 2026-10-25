#include "book_reader.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQuickWebEngineProfile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>

#include "book.h"
#include "book_library.h"
#include "book_search.h"
#include "cousoir_paths.h"
#include "epub_book.h"
#include "highlight_json.h"
#include "kobo_highlights.h"

namespace {

const QByteArray scheme = "cousoir-book";

// The books open, by the host of their addresses.
QMutex booksLock;
QHash<QString, std::weak_ptr<EpubBook>> openBooks;

// Serves the files of the books open, as WebEngine asks for them.
class SchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        std::shared_ptr<EpubBook> book;
        {
            const QMutexLocker locker(&booksLock);
            book = openBooks.value(job->requestUrl().host()).lock();
        }
        const QString name = job->requestUrl().path(QUrl::FullyDecoded).mid(1);
        if (!book || !book->contains(name)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        auto *buffer = new QBuffer(job);
        buffer->setData(book->read(name));
        buffer->open(QIODevice::ReadOnly);
        QString type = book->mediaType(name);
        // XHTML read as HTML would lose its namespaces, SVG and MathML.
        if (type.isEmpty())
            type = QStringLiteral("application/octet-stream");
        job->reply(type.toUtf8(), buffer);
    }
};

void installHandler()
{
    static SchemeHandler *handler = nullptr;
    if (handler)
        return;
    handler = new SchemeHandler(QQuickWebEngineProfile::defaultProfile());
    QQuickWebEngineProfile::defaultProfile()->installUrlSchemeHandler(scheme, handler);
}

int spineStep(const QString &cfi)
{
    static const QRegularExpression step(QStringLiteral("^epubcfi\\(/(\\d+)"));
    const QRegularExpressionMatch match = step.match(cfi);
    return match.hasMatch() ? match.captured(1).toInt() / 2 - 1 : -1;
}

} // namespace

BookReader::BookReader(QObject *parent) : QObject(parent)
{
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(5000);
    connect(&m_flushTimer, &QTimer::timeout, this, [this] { flush(false); });
}

BookReader::~BookReader()
{
    close();
}

namespace {
bool initialized = false;
}

void BookReader::initialize()
{
    QWebEngineUrlScheme definition(scheme);
    definition.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    definition.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme
                        | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(definition);
    QtWebEngineQuick::initialize();
    initialized = true;
}

bool BookReader::ready()
{
    return initialized;
}

BookLibrary *BookReader::library() const
{
    return m_library;
}

void BookReader::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    if (m_library)
        disconnect(m_library, &BookLibrary::changeFinished, this, nullptr);
    m_library = library;
    if (m_library)
        connect(m_library, &BookLibrary::changeFinished, this, &BookReader::finishSave);
    emit libraryChanged();
}

int BookReader::bookId() const
{
    return m_bookId;
}

QString BookReader::title() const
{
    return m_book ? m_book->title() : m_pdfTitle;
}

QString BookReader::format() const
{
    return m_format;
}

qint64 BookReader::totalSize() const
{
    qint64 total = 0;
    if (m_book) {
        for (const EpubBook::SpineItem &item : m_book->spine())
            total += item.size;
    }
    return total;
}

QVariantList BookReader::spine() const
{
    QVariantList items;
    if (!m_book)
        return items;
    for (const EpubBook::SpineItem &item : m_book->spine()) {
        QUrl url;
        url.setScheme(QString::fromLatin1(scheme));
        url.setHost(m_token);
        url.setPath(u'/' + item.name);
        items.append(QVariantMap{ { QStringLiteral("url"), url },
                                  { QStringLiteral("name"), item.name },
                                  { QStringLiteral("size"), item.size } });
    }
    return items;
}

QVariantList BookReader::toc() const
{
    QVariantList entries;
    if (!m_book)
        return entries;
    for (const EpubBook::TocEntry &entry : m_book->toc()) {
        entries.append(QVariantMap{ { QStringLiteral("title"), entry.title },
                                    { QStringLiteral("spineIndex"), m_book->spineIndex(entry.name) },
                                    { QStringLiteral("fragment"), entry.fragment },
                                    { QStringLiteral("depth"), entry.depth } });
    }
    return entries;
}

QVariantMap BookReader::startPosition() const
{
    return m_start;
}

QString BookReader::script() const
{
    QFile file(QStringLiteral(":/qt/qml/Cousoir/assets/reader/reader.js"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QVariantList BookReader::highlights() const
{
    QList<QJsonObject> sorted = m_highlights.values();
    // The book's order: by spine item, then where they start.
    std::stable_sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QLatin1String("spine_index")).toInt() < b.value(QLatin1String("spine_index")).toInt();
    });
    QVariantList list;
    for (const QJsonObject &highlight : std::as_const(sorted)) {
        const QJsonObject style = highlight.value(QLatin1String("style")).toObject();
        const QPair<QString, QString> owner = m_owners.value(highlight.value(QLatin1String("uuid")).toString());
        int spineIndex = highlight.value(QLatin1String("spine_index")).toInt(-1);
        const QString spineName = highlight.value(QLatin1String("spine_name")).toString();
        if (m_book && !spineName.isEmpty() && m_book->spineIndex(spineName) >= 0)
            spineIndex = m_book->spineIndex(spineName);
        list.append(QVariantMap{
            { QStringLiteral("uuid"), highlight.value(QLatin1String("uuid")).toString() },
            { QStringLiteral("spineIndex"), spineIndex },
            { QStringLiteral("start"), highlight.value(QLatin1String("start_cfi")).toString() },
            { QStringLiteral("end"), highlight.value(QLatin1String("end_cfi")).toString() },
            { QStringLiteral("text"), highlight.value(QLatin1String("highlighted_text")).toString() },
            { QStringLiteral("notes"), highlight.value(QLatin1String("notes")).toString() },
            { QStringLiteral("which"), style.value(QLatin1String("which")).toString(QStringLiteral("yellow")) },
            { QStringLiteral("timestamp"), highlight.value(QLatin1String("timestamp")).toString() },
            { QStringLiteral("source"), owner.first == QLatin1String("kobo") ? QStringLiteral("kobo:") + owner.second : QString() },
            { QStringLiteral("sourceName"), owner.first == QLatin1String("kobo") ? m_koboNames.value(owner.second) : QString() },
        });
    }
    return list;
}

QString BookReader::addHighlight(int spineIndex, const QString &start, const QString &end, const QString &text,
                                 const QString &which)
{
    if (!m_book || spineIndex < 0 || spineIndex >= m_book->spine().size() || start.isEmpty() || end.isEmpty())
        return {};
    const QString uuid = QUuid::createUuid().toString(QUuid::Id128);
    // The table of contents entries the highlight is under, as the viewer
    // notes them.
    const QJsonObject highlight = HighlightJson::highlight(
        uuid, HighlightJson::isoTimestamp(QDateTime::currentDateTimeUtc()), text, start, end, which,
        m_book->spine().at(spineIndex).name, spineIndex, HighlightJson::tocFamily(*m_book, spineIndex));
    m_highlights.insert(uuid, highlight);
    emit highlightsChanged();
    queueAnnotation({ m_format, QStringLiteral("highlight"), uuid, double(QDateTime::currentMSecsSinceEpoch()) / 1000,
                      QJsonDocument(highlight).toJson(QJsonDocument::Compact), HighlightJson::searchableText(highlight) });
    return uuid;
}

void BookReader::removeHighlight(const QString &uuid)
{
    if (!m_highlights.contains(uuid))
        return;
    // remove_highlight() of the viewer: the id and type kept, removed, as
    // whose it was: a Kobo's stays out of the library when the Kobo syncs.
    const QJsonObject removed = HighlightJson::removed(uuid, HighlightJson::isoTimestamp(QDateTime::currentDateTimeUtc()));
    const QPair<QString, QString> owner = m_owners.value(uuid, { QStringLiteral("local"), QStringLiteral("viewer") });
    m_highlights.remove(uuid);
    m_owners.remove(uuid);
    emit highlightsChanged();
    queueAnnotation({ m_format, QStringLiteral("highlight"), uuid, double(QDateTime::currentMSecsSinceEpoch()) / 1000,
                      QJsonDocument(removed).toJson(QJsonDocument::Compact), QString(), owner.first, owner.second });
}

void BookReader::queueAnnotation(const Annotation &annotation)
{
    // The last change of a highlight replaces the one before.
    m_pending.removeIf([&annotation](const Annotation &pending) { return pending.id == annotation.id; });
    m_pending.append(annotation);
    keepPending();
    m_flushTimer.start();
}

void BookReader::keepPending() const
{
    keepPending(m_uuid);
}

void BookReader::keepPending(const QString &uuid) const
{
    // Those on their way to the library, then those still to write, the
    // last of each highlight.
    QList<Annotation> waiting;
    QList<int> ids = m_saving.keys();
    std::sort(ids.begin(), ids.end());
    for (const int id : std::as_const(ids)) {
        if (m_saving.value(id).first == uuid)
            waiting += m_saving.value(id).second;
    }
    if (uuid == m_uuid)
        waiting += m_pending;
    QJsonArray list;
    QStringList listed;
    for (qsizetype i = waiting.size() - 1; i >= 0; --i) {
        const Annotation &annotation = waiting.at(i);
        if (listed.contains(annotation.id))
            continue;
        listed.append(annotation.id);
        list.prepend(QJsonObject{ { QStringLiteral("format"), annotation.format },
                                  { QStringLiteral("type"), annotation.type },
                                  { QStringLiteral("id"), annotation.id },
                                  { QStringLiteral("timestamp"), annotation.timestamp },
                                  { QStringLiteral("data"), QString::fromUtf8(annotation.data) },
                                  { QStringLiteral("searchable"), annotation.searchable },
                                  { QStringLiteral("user_type"), annotation.userType },
                                  { QStringLiteral("user"), annotation.user } });
    }
    QJsonObject positions = readPositions();
    if (list.isEmpty())
        positions.remove(QStringLiteral("pending:") + uuid);
    else
        positions.insert(QStringLiteral("pending:") + uuid, list);
    writePositions(positions);
}

void BookReader::flush(bool withPosition)
{
    m_flushTimer.stop();
    if (!m_library || m_bookId <= 0)
        return;
    std::optional<ReadPosition> position;
    if (withPosition && !m_lastCfi.isEmpty() && m_lastCfi != m_start.value(QStringLiteral("cfi")).toString())
        position = ReadPosition{ m_format, QStringLiteral("local"), QStringLiteral("cousoir"), m_lastCfi,
                                 double(QDateTime::currentMSecsSinceEpoch()) / 1000, m_lastFraction };
    if (!position && m_pending.isEmpty())
        return;
    // While the library cannot be written, the highlights wait with the
    // positions, for the next time the book is read; and while they go, until
    // they are in.
    const int id = m_library->saveReading(m_bookId, position, m_pending);
    if (id < 0)
        return;
    if (id > 0)
        m_saving.insert(id, { m_uuid, m_pending });
    m_pending.clear();
}

void BookReader::finishSave(int id, bool saved)
{
    if (!m_saving.contains(id))
        return;
    const auto [uuid, written] = m_saving.take(id);
    // Not in: back with those to write, unless changed since, to write
    // again.
    if (!saved && uuid == m_uuid && m_book) {
        for (const Annotation &annotation : written) {
            if (std::none_of(m_pending.cbegin(), m_pending.cend(),
                             [&annotation](const Annotation &pending) { return pending.id == annotation.id; }))
                m_pending.append(annotation);
        }
        m_flushTimer.start();
    }
    // In, or waiting again: kept aside as they are now, the book open or not.
    keepPending(uuid);
}

QVariantList BookReader::searchResults() const
{
    return m_searchResults;
}

QString BookReader::searchQuery() const
{
    return m_searchQuery;
}

bool BookReader::searching() const
{
    return m_searching;
}

bool BookReader::searchMore() const
{
    return m_searchMore;
}

void BookReader::search(const QString &query)
{
    const int run = ++m_searchRun;
    m_searchQuery = query;
    m_searchResults.clear();
    m_searchMore = false;
    m_searching = m_search && !query.trimmed().isEmpty();
    emit searchChanged();
    if (!m_searching)
        return;
    struct Found
    {
        QList<BookSearch::Match> matches;
        bool more = false;
    };
    auto *watcher = new QFutureWatcher<Found>(this);
    connect(watcher, &QFutureWatcher<Found>::finished, this, [this, watcher, run] {
        watcher->deleteLater();
        if (run != m_searchRun)
            return;
        const Found found = watcher->result();
        for (qsizetype i = 0; i < found.matches.size(); ++i) {
            const BookSearch::Match &match = found.matches.at(i);
            const bool newChapter = i == 0 || found.matches.at(i - 1).chapter != match.chapter
                                    || (match.chapter.isEmpty() && found.matches.at(i - 1).spineIndex != match.spineIndex);
            m_searchResults.append(QVariantMap{ { QStringLiteral("spineIndex"), match.spineIndex },
                                                { QStringLiteral("newChapter"), newChapter },
                                                { QStringLiteral("start"), match.start },
                                                { QStringLiteral("end"), match.end },
                                                { QStringLiteral("text"), match.text },
                                                { QStringLiteral("before"), match.before },
                                                { QStringLiteral("after"), match.after },
                                                { QStringLiteral("chapter"), match.chapter } });
        }
        m_searchMore = found.more;
        m_searching = false;
        emit searchChanged();
    });
    watcher->setFuture(QtConcurrent::run([search = m_search, query] {
        Found found;
        found.matches = search->find(query, 1000, &found.more);
        return found;
    }));
}

QString BookReader::open(int bookId)
{
    close();
    const Book *book = m_library ? m_library->data().book(bookId) : nullptr;
    if (!book)
        return tr("This book is no longer in the library.");
    QString path;
    for (const char *wanted : { "EPUB", "KEPUB" }) {
        for (const BookFormat &format : book->formats) {
            if (path.isEmpty() && format.format == QLatin1String(wanted) && !format.file.isEmpty()) {
                path = format.file;
                m_format = format.format;
            }
        }
    }
    if (path.isEmpty())
        return tr("“%1” has no EPUB to read.").arg(book->title);
    QString error;
    std::unique_ptr<EpubBook> opened = EpubBook::open(path, &error);
    if (!opened)
        return error;
    installHandler();
    m_book = std::move(opened);
    m_search = std::make_shared<BookSearch>(m_book);
    m_bookId = bookId;
    m_uuid = book->uuid;
    m_token = QStringLiteral("book-%1").arg(bookId);
    {
        const QMutexLocker locker(&booksLock);
        openBooks.insert(m_token, m_book);
    }
    // Where reading stopped last: Cousoir's own position, or a newer one in
    // the library, of Calibre's viewer or its content server.
    const QJsonObject positions = readPositions();
    const QJsonObject kept = positions.value(m_uuid).toObject();
    QList<ReadPosition> candidates = m_library->data().readPositions.value(bookId);
    if (kept.value(QLatin1String("format")).toString() == m_format)
        candidates.append({ m_format, QStringLiteral("local"), QStringLiteral("cousoir"),
                            kept.value(QLatin1String("cfi")).toString(), kept.value(QLatin1String("epoch")).toDouble(),
                            kept.value(QLatin1String("fraction")).toDouble() });
    m_start = { { QStringLiteral("spineIndex"), 0 }, { QStringLiteral("cfi"), QString() }, { QStringLiteral("device"), QString() } };
    double newest = -1;
    for (const ReadPosition &position : std::as_const(candidates)) {
        const int index = spineStep(position.cfi);
        if (position.format != m_format || index < 0 || index >= m_book->spine().size() || position.epoch <= newest)
            continue;
        newest = position.epoch;
        m_start = { { QStringLiteral("spineIndex"), index },
                    { QStringLiteral("cfi"), position.cfi },
                    { QStringLiteral("device"), position.device } };
    }
    m_lastCfi.clear();
    // Calibre's viewer's highlights, those made here before, and the Kobos'.
    m_highlights.clear();
    m_owners.clear();
    for (const Annotation &annotation : m_library->annotations(bookId, m_format)) {
        const QJsonObject highlight = QJsonDocument::fromJson(annotation.data).object();
        if (annotation.type != QLatin1String("highlight") || highlight.value(QLatin1String("removed")).toBool())
            continue;
        m_highlights.insert(annotation.id, highlight);
        m_owners.insert(annotation.id, { annotation.userType, annotation.user });
        if (annotation.userType == QLatin1String("kobo") && !m_koboNames.contains(annotation.user)) {
            const std::optional<KoboHighlights::Snapshot> kobo = KoboHighlights::readSnapshot(annotation.user);
            m_koboNames.insert(annotation.user, kobo && !kobo->name.isEmpty() ? kobo->name : annotation.user);
        }
    }
    // Those made while the library could not be written, still to write.
    m_pending.clear();
    for (const QJsonValue &value : positions.value(QStringLiteral("pending:") + m_uuid).toArray()) {
        const QJsonObject kept = value.toObject();
        const Annotation annotation{ kept.value(QLatin1String("format")).toString(),
                                     kept.value(QLatin1String("type")).toString(),
                                     kept.value(QLatin1String("id")).toString(),
                                     kept.value(QLatin1String("timestamp")).toDouble(),
                                     kept.value(QLatin1String("data")).toString().toUtf8(),
                                     kept.value(QLatin1String("searchable")).toString(),
                                     kept.value(QLatin1String("user_type")).toString(QStringLiteral("local")),
                                     kept.value(QLatin1String("user")).toString(QStringLiteral("viewer")) };
        if (annotation.format != m_format)
            continue;
        m_pending.append(annotation);
        const QJsonObject highlight = QJsonDocument::fromJson(annotation.data).object();
        if (highlight.value(QLatin1String("removed")).toBool()) {
            m_highlights.remove(annotation.id);
            m_owners.remove(annotation.id);
        } else {
            m_highlights.insert(annotation.id, highlight);
            m_owners.insert(annotation.id, { annotation.userType, annotation.user });
        }
    }
    if (!m_pending.isEmpty())
        m_flushTimer.start();
    emit bookChanged();
    emit highlightsChanged();
    return {};
}

void BookReader::close()
{
    if (!m_pdfPath.isEmpty()) {
        m_pdfPath.clear();
        m_pdfTitle.clear();
        m_bookId = 0;
        m_format.clear();
        m_start.clear();
        emit bookChanged();
    }
    if (!m_book)
        return;
    // Where reading stopped and the highlights, in the library too, for
    // Calibre and for Cousoir on the computers sharing it.
    flush(true);
    m_pending.clear();
    m_lastCfi.clear();
    {
        const QMutexLocker locker(&booksLock);
        openBooks.remove(m_token);
    }
    m_book.reset();
    m_search.reset();
    m_bookId = 0;
    m_format.clear();
    m_start.clear();
    emit bookChanged();
    if (!m_searchQuery.isEmpty() || m_searching)
        search(QString());
}

QUrl BookReader::fileUrl() const
{
    return m_pdfPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(m_pdfPath);
}

QString BookReader::openPdf(int bookId)
{
    close();
    const Book *book = m_library ? m_library->data().book(bookId) : nullptr;
    if (!book)
        return tr("This book is no longer in the library.");
    for (const BookFormat &format : book->formats) {
        if (format.format == QLatin1String("PDF") && !format.file.isEmpty())
            m_pdfPath = format.file;
    }
    if (m_pdfPath.isEmpty())
        return tr("“%1” has no PDF to read.").arg(book->title);
    m_bookId = bookId;
    m_uuid = book->uuid;
    m_pdfTitle = book->title;
    m_format = QStringLiteral("PDF");
    const QJsonObject positions = readPositions();
    m_start = { { QStringLiteral("page"), positions.value(m_uuid + QStringLiteral(":PDF")).toObject().value(QLatin1String("page")).toInt() } };
    emit bookChanged();
    return {};
}

void BookReader::savePage(int page, int pages)
{
    if (m_pdfPath.isEmpty() || m_uuid.isEmpty())
        return;
    QJsonObject positions = readPositions();
    positions.insert(m_uuid + QStringLiteral(":PDF"),
                     QJsonObject{ { QStringLiteral("page"), page },
                                  { QStringLiteral("fraction"), pages > 0 ? double(page + 1) / pages : 0 },
                                  { QStringLiteral("epoch"), double(QDateTime::currentMSecsSinceEpoch()) / 1000 } });
    writePositions(positions);
}

int BookReader::spineIndexOf(const QUrl &url) const
{
    if (!m_book || url.scheme() != QLatin1String(scheme) || url.host() != m_token)
        return -1;
    return m_book->spineIndex(url.path(QUrl::FullyDecoded).mid(1));
}

double BookReader::bookFraction(int spineIndex, double fileFraction) const
{
    if (!m_book || spineIndex < 0 || spineIndex >= m_book->spine().size())
        return 0;
    qint64 before = 0;
    qint64 total = 0;
    for (int i = 0; i < m_book->spine().size(); ++i) {
        if (i < spineIndex)
            before += m_book->spine().at(i).size;
        total += m_book->spine().at(i).size;
    }
    if (total <= 0)
        return 0;
    return std::clamp((double(before) + fileFraction * double(m_book->spine().at(spineIndex).size)) / double(total), 0.0, 1.0);
}

QVariantMap BookReader::locate(double bookFraction) const
{
    const qint64 total = totalSize();
    if (!m_book || m_book->spine().isEmpty())
        return { { QStringLiteral("spineIndex"), -1 }, { QStringLiteral("fileFraction"), 0 } };
    const double wanted = std::clamp(bookFraction, 0.0, 1.0) * double(total);
    qint64 before = 0;
    const int last = int(m_book->spine().size()) - 1;
    for (int i = 0; i <= last; ++i) {
        const qint64 size = m_book->spine().at(i).size;
        if (wanted < double(before + size) || i == last) {
            const double fraction = size > 0 ? (wanted - double(before)) / double(size) : 0;
            return { { QStringLiteral("spineIndex"), i }, { QStringLiteral("fileFraction"), std::clamp(fraction, 0.0, 1.0) } };
        }
        before += size;
    }
    return { { QStringLiteral("spineIndex"), last }, { QStringLiteral("fileFraction"), 1.0 } };
}

void BookReader::savePosition(int spineIndex, const QString &cfi, double fileFraction)
{
    if (!m_book || m_uuid.isEmpty() || cfi.isEmpty())
        return;
    m_lastCfi = cfi;
    m_lastFraction = bookFraction(spineIndex, fileFraction);
    QJsonObject positions = readPositions();
    positions.insert(m_uuid, QJsonObject{
                                 { QStringLiteral("format"), m_format },
                                 { QStringLiteral("cfi"), cfi },
                                 { QStringLiteral("fraction"), m_lastFraction },
                                 { QStringLiteral("epoch"), double(QDateTime::currentMSecsSinceEpoch()) / 1000 },
                             });
    writePositions(positions);
}

QJsonObject BookReader::readPositions() const
{
    QFile file(positionsFile());
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

void BookReader::writePositions(const QJsonObject &positions) const
{
    const QString path = positionsFile();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(positions).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

QString BookReader::positionsFile() const
{
    if (!m_library)
        return {};
    // Beside what the readers told of the books' reading.
    QString file = CousoirPaths::readingFile(m_library->libraryPath());
    file.replace(QRegularExpression(QStringLiteral("\\.json$")), QStringLiteral("-positions.json"));
    return file;
}
