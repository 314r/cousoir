#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

#include "library_server.h"

class BookLibrary;

// The open library shared on the local network, for the application: a
// LibraryServer fed the library's books as they load, started while
// `enabled` is on.
class LibraryShare : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int port READ port WRITE setPort NOTIFY portChanged)
    // Empty for none.
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY passwordChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    // The addresses other devices open: the web page's, then the catalog's.
    Q_PROPERTY(QStringList urls READ urls NOTIFY stateChanged)
    Q_PROPERTY(QString catalogPath READ catalogPath CONSTANT)
    // Why the server could not start.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    // Calibre's content server takes 8080: another port, so both can run.
    static constexpr int defaultPort = 8090;

    explicit LibraryShare(QObject *parent = nullptr);

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    bool enabled() const;
    void setEnabled(bool enabled);
    int port() const;
    void setPort(int port);
    QString password() const;
    void setPassword(const QString &password);
    bool running() const;
    QStringList urls() const;
    QString catalogPath() const;
    QString error() const;

    // Looks for the network's addresses again, after a change of network.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void copy(const QString &text) const;
    // A QR code of `text` as an image source, for a phone's camera.
    Q_INVOKABLE QString qrCode(const QString &text) const;

signals:
    void libraryChanged();
    void enabledChanged();
    void portChanged();
    void passwordChanged();
    void stateChanged();

private:
    void updateCatalog();
    void restart();

    LibraryServer m_server;
    QPointer<BookLibrary> m_library;
    bool m_enabled = false;
    int m_port = defaultPort;
    QString m_password;
    QStringList m_urls;
    QString m_error;
};
