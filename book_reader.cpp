#include "book_reader.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickWebEngineProfile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>

#include "book.h"
#include "book_library.h"
#include "book_import.h"
#include "book_search.h"
#ifdef COUSOIR_SPEECH
#include "book_speech.h"
#endif
#include "calibre_job.h"
#include "cousoir_paths.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "highlight_json.h"
#include "kobo_highlights.h"
#include "mobi_book.h"
#include "xml_tree.h"

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

// The book's pictures for QML: image://book/<host>/<name in the archive>.
class ImageProvider : public QQuickImageProvider
{
public:
    ImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        const qsizetype slash = id.indexOf(u'/');
        std::shared_ptr<EpubBook> book;
        {
            const QMutexLocker locker(&booksLock);
            book = openBooks.value(id.left(slash)).lock();
        }
        QImage image;
        if (book && slash > 0)
            image = QImage::fromData(book->read(QUrl::fromPercentEncoding(id.mid(slash + 1).toUtf8())));
        if (size)
            *size = image.size();
        if (!image.isNull() && requestedSize.isValid() && !requestedSize.isEmpty())
            image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return image;
    }
};

// The note's elements kept as rich text, the others' text alone.
void appendNote(const XmlTree::Node &node, QString *html, QString *text)
{
    for (const auto &child : node.children) {
        if (child->kind == XmlTree::Node::Kind::Text || child->kind == XmlTree::Node::Kind::Cdata) {
            html->append(child->text.toHtmlEscaped());
            text->append(child->text);
            continue;
        }
        if (child->kind != XmlTree::Node::Kind::Element)
            continue;
        const QString name = child->localName.toLower();
        const QString type = child->attribute(QStringLiteral("epub:type")) + u' ' + child->attribute(QStringLiteral("role"));
        if (name == QLatin1String("script") || name == QLatin1String("style")
            || (name == QLatin1String("a") && type.contains(QLatin1String("backlink"))))
            continue;
        if (name == QLatin1String("br")) {
            html->append(QStringLiteral("<br/>"));
            text->append(u'\n');
            continue;
        }
        static const QHash<QString, QString> tags{
            { QStringLiteral("i"), QStringLiteral("i") },        { QStringLiteral("em"), QStringLiteral("i") },
            { QStringLiteral("cite"), QStringLiteral("i") },     { QStringLiteral("var"), QStringLiteral("i") },
            { QStringLiteral("dfn"), QStringLiteral("i") },      { QStringLiteral("b"), QStringLiteral("b") },
            { QStringLiteral("strong"), QStringLiteral("b") },   { QStringLiteral("sup"), QStringLiteral("sup") },
            { QStringLiteral("sub"), QStringLiteral("sub") },    { QStringLiteral("p"), QStringLiteral("p") },
            { QStringLiteral("div"), QStringLiteral("p") },      { QStringLiteral("li"), QStringLiteral("p") },
            { QStringLiteral("dd"), QStringLiteral("p") },       { QStringLiteral("blockquote"), QStringLiteral("p") },
        };
        const QString tag = tags.value(name);
        if (!tag.isEmpty())
            html->append(u'<' + tag + u'>');
        appendNote(*child, html, text);
        if (!tag.isEmpty())
            html->append(QStringLiteral("</") + tag + u'>');
        if (tag == QLatin1String("p"))
            text->append(u'\n');
    }
}

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

// What orders places by their CFIs, as cfi_sort_key() does: the steps of
// the path, then the offset in the text.
QList<int> cfiKey(const QString &cfi)
{
    static const QRegularExpression number(QStringLiteral("(/|:)(\\d+)(?:\\[[^\\]]*\\])?"));
    QList<int> key;
    int offset = 0;
    for (QRegularExpressionMatchIterator it = number.globalMatch(cfi); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (match.captured(1) == QLatin1String(":"))
            offset = match.captured(2).toInt();
        else
            key.append(match.captured(2).toInt());
    }
    key.append(-1);
    key.append(offset);
    return key;
}

} // namespace

BookReader::BookReader(QObject *parent) : QObject(parent)
{
#ifdef COUSOIR_SPEECH
    m_speech = new BookSpeech(this);
#endif
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

QString BookReader::language() const
{
    return m_book ? m_book->language() : QString();
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
    // The book's order: by spine item, then where they start.
    QList<std::pair<QList<int>, QJsonObject>> sorted;
    for (const QJsonObject &highlight : m_highlights) {
        QList<int> key = cfiKey(highlight.value(QLatin1String("start_cfi")).toString());
        key.prepend(highlight.value(QLatin1String("spine_index")).toInt());
        sorted.append({ key, highlight });
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QVariantList list;
    for (const auto &[key, highlight] : std::as_const(sorted)) {
        const QJsonObject style = highlight.value(QLatin1String("style")).toObject();
        const QPair<QString, QString> owner = m_owners.value(highlight.value(QLatin1String("uuid")).toString());
        int spineIndex = highlight.value(QLatin1String("spine_index")).toInt(-1);
        const QString spineName = highlight.value(QLatin1String("spine_name")).toString();
        const QJsonArray family = highlight.value(QLatin1String("toc_family_titles")).toArray();
        if (m_book && !spineName.isEmpty() && m_book->spineIndex(spineName) >= 0)
            spineIndex = m_book->spineIndex(spineName);
        list.append(QVariantMap{
            { QStringLiteral("uuid"), highlight.value(QLatin1String("uuid")).toString() },
            { QStringLiteral("spineIndex"), spineIndex },
            { QStringLiteral("start"), highlight.value(QLatin1String("start_cfi")).toString() },
            { QStringLiteral("end"), highlight.value(QLatin1String("end_cfi")).toString() },
            { QStringLiteral("text"), highlight.value(QLatin1String("highlighted_text")).toString() },
            { QStringLiteral("notes"), highlight.value(QLatin1String("notes")).toString() },
            { QStringLiteral("chapter"), family.isEmpty() ? QString() : family.last().toString() },
            { QStringLiteral("which"), style.value(QLatin1String("which")).toString(QStringLiteral("yellow")) },
            { QStringLiteral("timestamp"), highlight.value(QLatin1String("timestamp")).toString() },
            { QStringLiteral("source"), owner.first == QLatin1String("kobo") ? QStringLiteral("kobo:") + owner.second : QString() },
            { QStringLiteral("sourceName"), owner.first == QLatin1String("kobo") ? m_koboNames.value(owner.second) : QString() },
        });
    }
    return list;
}

QVariantList BookReader::bookmarks() const
{
    QList<std::pair<QList<int>, QJsonObject>> sorted;
    for (const QJsonObject &bookmark : m_bookmarks)
        sorted.append({ cfiKey(bookmark.value(QLatin1String("pos")).toString()), bookmark });
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QVariantList list;
    for (const auto &[key, bookmark] : std::as_const(sorted)) {
        const QString cfi = bookmark.value(QLatin1String("pos")).toString();
        list.append(QVariantMap{ { QStringLiteral("title"), bookmark.value(QLatin1String("title")).toString() },
                                 { QStringLiteral("cfi"), cfi },
                                 { QStringLiteral("spineIndex"), spineStep(cfi) },
                                 { QStringLiteral("timestamp"), bookmark.value(QLatin1String("timestamp")).toString() } });
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
    // notes them: those of its page before it.
    const QString name = m_book->spine().at(spineIndex).name;
    QStringList family = HighlightJson::tocFamily(*m_book, spineIndex);
    if (const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(m_book->read(name))) {
        const EpubCfi::Document document(*root);
        const EpubCfi::PageText page = EpubCfi::PageText::of(*root);
        const int order = page.order.value(document.decode(start).node, -1);
        family = HighlightJson::tocFamily(*m_book, spineIndex, [&](const QString &fragment) {
            const XmlTree::Node *target = document.element(fragment);
            return target && page.order.value(target, -1) <= order;
        });
    }
    const QJsonObject highlight = HighlightJson::highlight(
        uuid, HighlightJson::isoTimestamp(QDateTime::currentDateTimeUtc()), text, start, end, which, name, spineIndex,
        family);
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

void BookReader::setHighlightStyle(const QString &uuid, const QString &which)
{
    if (!m_highlights.contains(uuid))
        return;
    QJsonObject &highlight = m_highlights[uuid];
    highlight.insert(QStringLiteral("style"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("builtin") },
                                                           { QStringLiteral("kind"), QStringLiteral("color") },
                                                           { QStringLiteral("which"), which } });
    highlight.insert(QStringLiteral("timestamp"), HighlightJson::isoTimestamp(QDateTime::currentDateTimeUtc()));
    emit highlightsChanged();
    queueHighlight(uuid);
}

void BookReader::setHighlightNotes(const QString &uuid, const QString &notes)
{
    if (!m_highlights.contains(uuid))
        return;
    QJsonObject &highlight = m_highlights[uuid];
    const QString trimmed = notes.trimmed();
    if (highlight.value(QLatin1String("notes")).toString() == trimmed)
        return;
    if (trimmed.isEmpty())
        highlight.remove(QStringLiteral("notes"));
    else
        highlight.insert(QStringLiteral("notes"), trimmed);
    highlight.insert(QStringLiteral("timestamp"), HighlightJson::isoTimestamp(QDateTime::currentDateTimeUtc()));
    emit highlightsChanged();
    queueHighlight(uuid);
}

void BookReader::queueHighlight(const QString &uuid)
{
    const QJsonObject highlight = m_highlights.value(uuid);
    const QPair<QString, QString> owner = m_owners.value(uuid, { QStringLiteral("local"), QStringLiteral("viewer") });
    queueAnnotation({ m_format, QStringLiteral("highlight"), uuid, double(QDateTime::currentMSecsSinceEpoch()) / 1000,
                      QJsonDocument(highlight).toJson(QJsonDocument::Compact), HighlightJson::searchableText(highlight),
                      owner.first, owner.second });
}

void BookReader::addBookmark(const QString &title, const QString &cfi)
{
    const QString name = title.trimmed();
    if (!m_book || name.isEmpty() || cfi.isEmpty())
        return;
    const QJsonObject bookmark{ { QStringLiteral("type"), QStringLiteral("bookmark") },
                                { QStringLiteral("timestamp"), HighlightJson::isoTimestamp(QDateTime::currentDateTimeUtc()) },
                                { QStringLiteral("pos_type"), QStringLiteral("epubcfi") },
                                { QStringLiteral("pos"), cfi },
                                { QStringLiteral("title"), name } };
    m_bookmarks.insert(name, bookmark);
    emit bookmarksChanged();
    queueBookmark(bookmark);
}

void BookReader::removeBookmark(const QString &title)
{
    if (!m_bookmarks.contains(title))
        return;
    QJsonObject bookmark = m_bookmarks.take(title);
    bookmark.insert(QStringLiteral("removed"), true);
    bookmark.insert(QStringLiteral("timestamp"), HighlightJson::isoTimestamp(QDateTime::currentDateTimeUtc()));
    emit bookmarksChanged();
    queueBookmark(bookmark);
}

bool BookReader::renameBookmark(const QString &title, const QString &newTitle)
{
    const QString name = newTitle.trimmed();
    if (!m_bookmarks.contains(title) || name.isEmpty() || (name != title && m_bookmarks.contains(name)))
        return false;
    if (name == title)
        return true;
    const QString cfi = m_bookmarks.value(title).value(QLatin1String("pos")).toString();
    removeBookmark(title);
    addBookmark(name, cfi);
    return true;
}

void BookReader::queueBookmark(const QJsonObject &bookmark)
{
    const QString title = bookmark.value(QLatin1String("title")).toString();
    queueAnnotation({ m_format, QStringLiteral("bookmark"), title, double(QDateTime::currentMSecsSinceEpoch()) / 1000,
                      QJsonDocument(bookmark).toJson(QJsonDocument::Compact),
                      bookmark.value(QLatin1String("removed")).toBool() ? QString() : title.normalized(QString::NormalizationForm_KC) });
}

void BookReader::queueAnnotation(const Annotation &annotation)
{
    // The last change of an annotation replaces the one before.
    m_pending.removeIf([&annotation](const Annotation &pending) {
        return pending.id == annotation.id && pending.type == annotation.type;
    });
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
    QSet<QString> listed;
    for (qsizetype i = waiting.size() - 1; i >= 0; --i) {
        const Annotation &annotation = waiting.at(i);
        const QString key = annotation.type + u':' + annotation.id;
        if (listed.contains(key))
            continue;
        listed.insert(key);
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
    // A Kindle book's highlights and bookmarks are Cousoir's own, kept with
    // its positions: Calibre's viewer would look for them in its own
    // conversion of the book.
    if (m_local) {
        if (m_pending.isEmpty())
            return;
        QJsonObject positions = readPositions();
        const QString key = QStringLiteral("annotations:%1:%2").arg(m_uuid, m_format);
        QJsonObject kept = positions.value(key).toObject();
        for (const Annotation &annotation : std::as_const(m_pending)) {
            kept.insert(annotation.type + u':' + annotation.id,
                        QJsonObject{ { QStringLiteral("type"), annotation.type },
                                     { QStringLiteral("id"), annotation.id },
                                     { QStringLiteral("data"), QString::fromUtf8(annotation.data) } });
        }
        positions.insert(key, kept);
        positions.remove(QStringLiteral("pending:") + m_uuid);
        writePositions(positions);
        m_pending.clear();
        return;
    }
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
            if (std::none_of(m_pending.cbegin(), m_pending.cend(), [&annotation](const Annotation &pending) {
                    return pending.id == annotation.id && pending.type == annotation.type;
                }))
                m_pending.append(annotation);
        }
        m_flushTimer.start();
    }
    // In, or waiting again: kept aside as they are now, the book open or not.
    keepPending(uuid);
}

QVariantMap BookReader::note(const QUrl &url) const
{
    if (!m_book || url.scheme() != QLatin1String(scheme) || url.host() != m_token || !url.hasFragment())
        return {};
    const QString name = url.path(QUrl::FullyDecoded).mid(1);
    const QString id = url.fragment(QUrl::FullyDecoded);
    const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(m_book->read(name));
    if (!root || id.isEmpty())
        return {};
    const EpubCfi::Document document(*root);
    const XmlTree::Node *target = document.element(id);
    if (!target)
        return {};
    // The note: the element named when it is a block, else the block it is
    // in, as an anchor at the start of Gutenberg's notes is.
    static const QSet<QString> blocks{
        QStringLiteral("aside"), QStringLiteral("li"), QStringLiteral("p"), QStringLiteral("div"), QStringLiteral("dd"),
        QStringLiteral("dt"), QStringLiteral("section"), QStringLiteral("blockquote"), QStringLiteral("td"),
        QStringLiteral("footer"),
    };
    const XmlTree::Node *block = target;
    while (block && !(block->kind == XmlTree::Node::Kind::Element && blocks.contains(block->localName.toLower()))) {
        block = block->parent;
        if (block && block->isElement("body"))
            block = nullptr;
    }
    if (!block)
        block = target;
    QString html;
    QString text;
    appendNote(*block, &html, &text);
    text = text.simplified();
    // A section, a chapter: no note to show in a box. An EPUB 3 footnote
    // is one however long, the page hiding it.
    const QString type = block->attribute(QStringLiteral("epub:type")) + u' ' + block->attribute(QStringLiteral("role"));
    const bool aside = block->isElement("aside") && (type.contains(QLatin1String("note")));
    if (text.isEmpty() || text.size() > (aside ? 100000 : 3000))
        return {};
    return { { QStringLiteral("html"), html.trimmed() }, { QStringLiteral("text"), text } };
}

QUrl BookReader::imageUrl(const QUrl &url) const
{
    if (!m_book || url.scheme() != QLatin1String(scheme) || url.host() != m_token)
        return {};
    return QUrl(QStringLiteral("image://book/") + m_token + url.path(QUrl::FullyEncoded));
}

bool BookReader::preparing() const
{
    return m_conversion != nullptr || m_kindle != nullptr;
}

bool BookReader::preparingCalibre() const
{
    return m_conversion != nullptr;
}

void BookReader::stopPreparing()
{
    const bool was = preparing();
    if (m_conversion) {
        QProcess *process = m_conversion;
        m_conversion = nullptr;
        process->disconnect(this);
        CalibreJob::stopProcessGroup(process);
        process->deleteLater();
        QFile::remove(m_preparingPart);
        QDir(m_preparingFolder).removeRecursively();
    }
    // A Kindle book's thread goes on to its end; its result is let go then.
    m_kindle = nullptr;
    m_preparingBook = 0;
    m_preparingTitle.clear();
    if (was)
        emit preparingChanged();
}

void BookReader::convertKindle(int bookId, const QString &title, const QString &source, const QString &part,
                               const QString &made)
{
    stopPreparing();
    auto *watcher = new QFutureWatcher<QString>(this);
    m_kindle = watcher;
    m_preparingBook = bookId;
    m_preparingTitle = title;
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, bookId, part, made, title] {
        watcher->deleteLater();
        QString error = watcher->result();
        if (m_kindle != watcher) {
            QFile::remove(part);
            return;
        }
        m_kindle = nullptr;
        m_preparingBook = 0;
        m_preparingTitle.clear();
        if (error.isEmpty() && !QFile::rename(part, made))
            error = tr("“%1” cannot be read.").arg(title);
        if (!error.isEmpty())
            QFile::remove(part);
        emit preparingChanged();
        emit prepared(bookId, error);
    });
    // On a thread: a large book takes a moment, a damaged one longer.
    watcher->setFuture(QtConcurrent::run([source, part, title] {
        QString error;
        if (MobiBook::toEpub(source, part, &error))
            return QString();
        return error.isEmpty() ? tr("“%1” cannot be read.").arg(title) : error;
    }));
    emit preparingChanged();
}

QString BookReader::preparingTitle() const
{
    return m_preparingTitle;
}

QStringList BookReader::convertedFormats()
{
    return { QStringLiteral("FB2"),  QStringLiteral("FBZ"),  QStringLiteral("DOCX"), QStringLiteral("ODT"),
             QStringLiteral("RTF"),  QStringLiteral("HTMLZ"), QStringLiteral("HTML"), QStringLiteral("TXTZ"),
             QStringLiteral("TXT"),  QStringLiteral("LIT"),  QStringLiteral("PDB"),  QStringLiteral("PRC"),
             QStringLiteral("LRF"),  QStringLiteral("CHM"),  QStringLiteral("SNB"),  QStringLiteral("RB"),
             QStringLiteral("TCR"),  QStringLiteral("PML"),  QStringLiteral("CBZ"),  QStringLiteral("CBR"),
             QStringLiteral("CB7"),  QStringLiteral("CBC") };
}

QString BookReader::converter()
{
    const QString fake = qEnvironmentVariable("COUSOIR_EBOOK_CONVERT");
    if (!fake.isEmpty())
        return fake;
    const QString debug = BookImport::calibreDebug();
    if (debug.isEmpty())
        return {};
    const QString program = QFileInfo(debug).dir().filePath(
        QStringLiteral("ebook-convert") + (debug.endsWith(QLatin1String(".exe")) ? QStringLiteral(".exe") : QString()));
    return QFileInfo(program).isExecutable() ? program : QString();
}

QObject *BookReader::speech() const
{
    return m_speech;
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

void BookReader::copyText(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
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
    QStringList wanted{ QStringLiteral("EPUB"), QStringLiteral("KEPUB"), QStringLiteral("AZW3"), QStringLiteral("MOBI"),
                        QStringLiteral("AZW") };
    if (!converter().isEmpty())
        wanted += convertedFormats();
    for (const QString &name : std::as_const(wanted)) {
        for (const BookFormat &format : book->formats) {
            if (path.isEmpty() && format.format == name && !format.file.isEmpty()) {
                path = format.file;
                m_format = format.format;
            }
        }
    }
    if (path.isEmpty())
        return tr("“%1” has no EPUB to read.").arg(book->title);
    QString error;
    // Another format, made an EPUB once, by Cousoir for a Kindle book, by
    // Calibre for the others, kept in the cache while the book is
    // unchanged.
    m_local = m_format != QLatin1String("EPUB") && m_format != QLatin1String("KEPUB");
    if (m_local) {
        const QFileInfo info(path);
        const QDir cache(CousoirPaths::cacheFolder(m_library->libraryPath()) + QStringLiteral("/reader"));
        const QString prefix = QStringLiteral("%1-%2-").arg(book->uuid, m_format);
        // The maker's version too: a book made by an older Cousoir is made
        // again.
        const bool calibre = convertedFormats().contains(m_format);
        const QString maker = calibre ? QStringLiteral("calibre") : QStringLiteral("cousoir%1").arg(MobiBook::version);
        const QString made = cache.filePath(prefix + QStringLiteral("%1-%2-%3.epub")
                                                         .arg(info.size())
                                                         .arg(info.lastModified().toMSecsSinceEpoch())
                                                         .arg(maker));
        if (!QFileInfo::exists(made)) {
            QDir().mkpath(cache.path());
            for (const QString &old : cache.entryList({ prefix + u'*' }, QDir::Files))
                QFile::remove(cache.filePath(old));
            const QString part = made + QStringLiteral(".part.epub");
            m_format.clear();
            if (calibre)
                return convert(bookId, book->title, path, part, made) ? QString() : tr("Calibre cannot be run.");
            convertKindle(bookId, book->title, path, part, made);
            return {};
        }
        path = made;
    }
    std::unique_ptr<EpubBook> opened = EpubBook::open(path, &error);
    if (!opened)
        return error;
    installHandler();
    // The book's pictures for the window's Image.
    if (QQmlEngine *engine = qmlEngine(this); engine && !engine->imageProvider(QStringLiteral("book")))
        engine->addImageProvider(QStringLiteral("book"), new ImageProvider);
    m_book = std::move(opened);
    m_search = std::make_shared<BookSearch>(m_book);
#ifdef COUSOIR_SPEECH
    m_speech->setBook(m_book, m_book->language());
#endif
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
    // A Kindle book's: Cousoir's alone, Calibre's viewer counting its places
    // in its own conversion.
    QList<ReadPosition> candidates = m_local ? QList<ReadPosition>() : m_library->data().readPositions.value(bookId);
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
    // Calibre's viewer's highlights, those made here before, and the Kobos';
    // the viewer's bookmarks, its own as it reads them.
    m_highlights.clear();
    m_owners.clear();
    m_bookmarks.clear();
    const auto isViewers = [](const Annotation &annotation) {
        return annotation.userType == QLatin1String("local") && annotation.user == QLatin1String("viewer");
    };
    QList<Annotation> annotations;
    if (m_local) {
        const QJsonObject kept = positions.value(QStringLiteral("annotations:%1:%2").arg(m_uuid, m_format)).toObject();
        for (const QJsonValue &value : kept) {
            const QJsonObject entry = value.toObject();
            annotations.append({ m_format, entry.value(QLatin1String("type")).toString(),
                                 entry.value(QLatin1String("id")).toString(), 0,
                                 entry.value(QLatin1String("data")).toString().toUtf8(), QString() });
        }
    } else {
        annotations = m_library->annotations(bookId, m_format);
    }
    for (const Annotation &annotation : std::as_const(annotations)) {
        const QJsonObject highlight = QJsonDocument::fromJson(annotation.data).object();
        if (highlight.value(QLatin1String("removed")).toBool())
            continue;
        if (annotation.type == QLatin1String("bookmark") && isViewers(annotation)) {
            m_bookmarks.insert(annotation.id, highlight);
            continue;
        }
        if (annotation.type != QLatin1String("highlight"))
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
        if (annotation.type == QLatin1String("bookmark")) {
            if (highlight.value(QLatin1String("removed")).toBool())
                m_bookmarks.remove(annotation.id);
            else
                m_bookmarks.insert(annotation.id, highlight);
        } else if (highlight.value(QLatin1String("removed")).toBool()) {
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
    emit bookmarksChanged();
    return {};
}

void BookReader::close()
{
    // A book being made an EPUB for the window: no longer wanted.
    stopPreparing();
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
#ifdef COUSOIR_SPEECH
    m_speech->setBook(nullptr, QString());
#endif
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

bool BookReader::convert(int bookId, const QString &title, const QString &source, const QString &part, const QString &made)
{
    stopPreparing();
    const QString program = converter();
    if (program.isEmpty())
        return false;
    auto *process = new QProcess(this);
    const QString folder = QFileInfo(made).dir().filePath(QStringLiteral("job-") + QFileInfo(made).completeBaseName());
    QDir().mkpath(folder);
    m_preparingPart = part;
    m_preparingFolder = folder;
    process->setProcessEnvironment(
        CalibreJob::environment(folder, m_library ? m_library->interfaceLanguage() : QString()));
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    m_conversion = process;
    m_preparingBook = bookId;
    m_preparingTitle = title;
    const auto finish = [this, process, bookId, part, made, folder, title](const QString &failure) {
        if (m_conversion != process)
            return;
        m_conversion = nullptr;
        m_preparingBook = 0;
        m_preparingTitle.clear();
        QString error = failure;
        if (error.isEmpty() && !QFile::rename(part, made))
            error = tr("Calibre could not make an EPUB of “%1”.").arg(title);
        if (!error.isEmpty())
            QFile::remove(part);
        QDir(folder).removeRecursively();
        process->deleteLater();
        emit preparingChanged();
        emit prepared(bookId, error);
    };
    connect(process, &QProcess::finished, this, [process, finish, title, part](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0 && QFileInfo::exists(part)) {
            finish(QString());
            return;
        }
        // Calibre's last words say why.
        const QStringList lines = QString::fromUtf8(process->readAll()).trimmed().split(u'\n');
        finish(tr("Calibre could not make an EPUB of “%1”: %2").arg(title, lines.isEmpty() ? QString() : lines.last().trimmed()));
    });
    connect(process, &QProcess::errorOccurred, this, [finish, title](QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart)
            finish(tr("Calibre cannot be run to read “%1”.").arg(title));
    });
    process->start(program, { source, part });
    emit preparingChanged();
    return true;
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
    for (int i = 0; i < spineIndex; ++i)
        before += m_book->spine().at(i).size;
    const qint64 total = totalSize();
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
