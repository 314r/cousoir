#include "app_instance.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace {

QStringList absolute(const QStringList &paths)
{
    QStringList result;
    for (const QString &path : paths) {
        // file:// addresses too, as file managers hand them.
        const QUrl url(path);
        const QString local = url.isLocalFile() ? url.toLocalFile() : path;
        result.append(QDir::cleanPath(QFileInfo(local).absoluteFilePath()));
    }
    return result;
}

QStringList strings(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray())
        result.append(item.toString());
    return result;
}

} // namespace

QVariantMap AppInstance::Invocation::toMap() const
{
    return { { QStringLiteral("files"), files },
             { QStringLiteral("add"), add },
             { QStringLiteral("addAsked"), addAsked },
             { QStringLiteral("library"), library },
             { QStringLiteral("show"), show } };
}

QByteArray AppInstance::Invocation::encode() const
{
    const QJsonObject object{ { QStringLiteral("v"), 1 },
                              { QStringLiteral("files"), QJsonArray::fromStringList(files) },
                              { QStringLiteral("add"), QJsonArray::fromStringList(add) },
                              { QStringLiteral("addAsked"), addAsked },
                              { QStringLiteral("library"), library },
                              { QStringLiteral("show"), show } };
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

AppInstance::Invocation AppInstance::Invocation::decode(const QByteArray &line)
{
    const QJsonObject object = QJsonDocument::fromJson(line.trimmed()).object();
    Invocation invocation;
    invocation.files = strings(object.value(QLatin1String("files")));
    invocation.add = strings(object.value(QLatin1String("add")));
    invocation.addAsked = object.value(QLatin1String("addAsked")).toBool();
    invocation.library = object.value(QLatin1String("library")).toString();
    invocation.show = object.value(QLatin1String("show")).toInt();
    return invocation;
}

AppInstance::AppInstance(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine())
                    return;
                const Invocation invocation = Invocation::decode(socket->readLine());
                socket->write("ok\n");
                socket->flush();
                emit invoked(invocation.toMap());
            });
        }
    });
}

bool AppInstance::parse(const QStringList &arguments, Invocation *invocation, QString *error)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Browse, edit and read a Calibre library."));
    const QCommandLineOption add(QStringLiteral("add"), QStringLiteral("Add the book files given to the library."));
    const QCommandLineOption library(QStringLiteral("library"), QStringLiteral("Open this library."),
                                     QStringLiteral("path"));
    const QCommandLineOption show(QStringLiteral("show"), QStringLiteral("Show the library's book of this id."),
                                  QStringLiteral("id"));
    parser.addOptions({ add, library, show });
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Book files to read."), QStringLiteral("[files…]"));
    if (!parser.parse(arguments)) {
        if (error)
            *error = parser.errorText();
        return false;
    }
    *invocation = {};
    const QStringList files = absolute(parser.positionalArguments());
    if (parser.isSet(add)) {
        invocation->addAsked = true;
        invocation->add = files;
    } else {
        invocation->files = files;
    }
    if (parser.isSet(library))
        invocation->library = absolute({ parser.value(library) }).constFirst();
    invocation->show = parser.value(show).toInt();
    return true;
}

QString AppInstance::serverName()
{
    const QByteArray cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation).toUtf8();
    return QStringLiteral("cousoir-%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(cache, QCryptographicHash::Sha1).toHex().left(16)));
}

AppInstance::Forward AppInstance::forward(const Invocation &invocation, const QString &name)
{
    QLocalSocket socket;
#ifdef Q_OS_LINUX
    socket.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
    socket.connectToServer(name);
    if (!socket.waitForConnected(1000))
        return Forward::NoInstance;
    socket.write(invocation.encode());
    if (!socket.waitForBytesWritten(2000))
        return Forward::NotAnswering;
    // The first Cousoir answers once it took the command line.
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(5000))
            return Forward::NotAnswering;
    }
    return socket.readLine().trimmed() == "ok" ? Forward::Taken : Forward::NotAnswering;
}

void AppInstance::focusWindow() const
{
    // Wayland lets a window take the focus only when the compositor agrees;
    // Hyprland is asked by the window's process.
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return;
    const QString hyprctl = QStandardPaths::findExecutable(QStringLiteral("hyprctl"));
    if (hyprctl.isEmpty())
        return;
    // Hyprland configured in Lua, as Omarchy's is, takes a dispatcher as Lua;
    // one configured as before takes its name and arguments, and says the
    // Lua is an error.
    const QString window = QStringLiteral("pid:%1").arg(QCoreApplication::applicationPid());
    auto *lua = new QProcess;
    QObject::connect(lua, &QProcess::finished, lua, [lua, hyprctl, window](int code) {
        const QByteArray answer = lua->readAll();
        if (code != 0 || answer.contains("error") || answer.contains("Invalid"))
            QProcess::startDetached(hyprctl, { QStringLiteral("dispatch"), QStringLiteral("focuswindow"), window });
        lua->deleteLater();
    });
    QObject::connect(lua, &QProcess::errorOccurred, lua, &QObject::deleteLater);
    lua->setProcessChannelMode(QProcess::MergedChannels);
    lua->start(hyprctl, { QStringLiteral("dispatch"), QStringLiteral("hl.dsp.focus({ window = \"%1\" })").arg(window) });
}

bool AppInstance::listen(const QString &name)
{
#ifdef Q_OS_LINUX
    // In the abstract namespace: nothing on the disk to stay after a crash.
    m_server.setSocketOptions(QLocalServer::AbstractNamespaceOption);
#else
    QLocalServer::removeServer(name);
#endif
    return m_server.listen(name);
}
