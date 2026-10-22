#include "library_share.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>

#include "book_library.h"
#include "qr_code.h"

LibraryShare::LibraryShare(QObject *parent) : QObject(parent) {}

BookLibrary *LibraryShare::library() const
{
    return m_library;
}

void LibraryShare::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (m_library) {
        connect(m_library, &BookLibrary::libraryChanged, this, &LibraryShare::updateCatalog);
        connect(m_library, &QObject::destroyed, this, [this] { m_server.setCatalog({}); });
    }
    updateCatalog();
    emit libraryChanged();
}

bool LibraryShare::enabled() const
{
    return m_enabled;
}

void LibraryShare::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    restart();
    emit enabledChanged();
}

int LibraryShare::port() const
{
    return m_port;
}

void LibraryShare::setPort(int port)
{
    port = std::clamp(port, 1, 65535);
    if (m_port == port)
        return;
    m_port = port;
    if (m_enabled)
        restart();
    emit portChanged();
}

QString LibraryShare::password() const
{
    return m_password;
}

void LibraryShare::setPassword(const QString &password)
{
    if (m_password == password)
        return;
    m_password = password;
    m_server.setPassword(password);
    emit passwordChanged();
}

bool LibraryShare::running() const
{
    return m_server.isListening();
}

QStringList LibraryShare::urls() const
{
    return m_urls;
}

QString LibraryShare::catalogPath() const
{
    return QStringLiteral("opds");
}

QString LibraryShare::error() const
{
    return m_error;
}

void LibraryShare::refresh()
{
    m_urls = running() ? LibraryServer::addresses(m_server.port()) : QStringList();
    emit stateChanged();
}

void LibraryShare::copy(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

QString LibraryShare::qrCode(const QString &text) const
{
    const QrCode::Matrix matrix = QrCode::encode(text.toUtf8());
    if (matrix.size == 0)
        return {};
    return QStringLiteral("data:image/svg+xml;base64,") + QString::fromLatin1(QrCode::svg(matrix).toBase64());
}

void LibraryShare::updateCatalog()
{
    if (!m_library || !m_library->loaded()) {
        m_server.setCatalog({});
        return;
    }
    const QPointer<BookLibrary> library = m_library;
    LibraryServer::Catalog catalog;
    catalog.data = &m_library->data();
    catalog.find = [library](const QString &query, const QList<BookSort::Level> &sort, QString *error) {
        return library ? library->findRows(query, sort, error) : QList<qsizetype>{};
    };
    catalog.name = QDir(m_library->libraryPath()).dirName();
    catalog.interfaceLanguage = m_library->interfaceLanguage();
    catalog.thumbnailFolder = BookLibrary::cacheFolder(m_library->libraryPath()) + QStringLiteral("/covers");
    m_server.setCatalog(catalog);
}

void LibraryShare::restart()
{
    m_server.close();
    m_error.clear();
    // The tests reach it from this computer only.
    if (m_enabled)
        m_error = m_server.listen(quint16(m_port), qEnvironmentVariableIsSet("COUSOIR_SHARE_LOCALHOST")
                                                       ? QHostAddress::LocalHost
                                                       : QHostAddress::Any);
    refresh();
}
