#include "book_reader.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQuickWebEngineProfile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>

#include "book.h"
#include "book_library.h"
#include "cousoir_paths.h"
#include "epub_book.h"

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

BookReader::BookReader(QObject *parent) : QObject(parent) {}

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
    m_library = library;
    emit libraryChanged();
}

int BookReader::bookId() const
{
    return m_bookId;
}

QString BookReader::title() const
{
    return m_book ? m_book->title() : QString();
}

QString BookReader::format() const
{
    return m_format;
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

QString BookReader::readableFormat(const QVariantList &formats)
{
    for (const char *wanted : { "EPUB", "KEPUB" }) {
        for (const QVariant &format : formats) {
            if (format.toMap().value(QStringLiteral("format")).toString() == QLatin1String(wanted))
                return QLatin1String(wanted);
        }
    }
    return {};
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
    m_bookId = bookId;
    m_uuid = book->uuid;
    m_token = QStringLiteral("book-%1").arg(bookId);
    {
        const QMutexLocker locker(&booksLock);
        openBooks.insert(m_token, m_book);
    }
    // Where reading stopped last: Cousoir's own position, or a newer one in
    // the library, of Calibre's viewer or its content server.
    QFile file(positionsFile());
    const QJsonObject positions = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                                                 : QJsonObject();
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
    emit bookChanged();
    return {};
}

void BookReader::close()
{
    if (!m_book)
        return;
    // Where reading stopped, in the library too, for Calibre and for Cousoir
    // on the computers sharing it.
    if (m_library && !m_lastCfi.isEmpty() && m_lastCfi != m_start.value(QStringLiteral("cfi")).toString())
        m_library->saveReadPosition(m_bookId, { m_format, QStringLiteral("local"), QStringLiteral("cousoir"), m_lastCfi,
                                                double(QDateTime::currentMSecsSinceEpoch()) / 1000, m_lastFraction });
    m_lastCfi.clear();
    {
        const QMutexLocker locker(&booksLock);
        openBooks.remove(m_token);
    }
    m_book.reset();
    m_bookId = 0;
    m_format.clear();
    m_start.clear();
    emit bookChanged();
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

void BookReader::savePosition(int spineIndex, const QString &cfi, double fileFraction)
{
    if (!m_book || m_uuid.isEmpty() || cfi.isEmpty())
        return;
    m_lastCfi = cfi;
    m_lastFraction = bookFraction(spineIndex, fileFraction);
    const QString path = positionsFile();
    QFile file(path);
    QJsonObject positions = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                                           : QJsonObject();
    file.close();
    positions.insert(m_uuid, QJsonObject{
                                 { QStringLiteral("format"), m_format },
                                 { QStringLiteral("cfi"), cfi },
                                 { QStringLiteral("fraction"), bookFraction(spineIndex, fileFraction) },
                                 { QStringLiteral("epoch"), double(QDateTime::currentMSecsSinceEpoch()) / 1000 },
                             });
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
