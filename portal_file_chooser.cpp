// The portal's file chooser, after Omacut's PortalFilePicker (MIT, David
// Heinemeier Hansson).
#include "portal_file_chooser.h"

#include <QRegularExpression>

#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>
#endif

PortalFileChooser::Filter PortalFileChooser::parseFilter(const QString &nameFilter)
{
    static const QRegularExpression pattern(QStringLiteral("^(.*?)\\s*\\(([^()]*)\\)\\s*$"));
    const QRegularExpressionMatch match = pattern.match(nameFilter);
    if (!match.hasMatch())
        return { nameFilter.trimmed(), { QStringLiteral("*") } };
    const QString name = match.captured(1).trimmed();
    return { name.isEmpty() ? match.captured(2).trimmed() : name,
             match.captured(2).split(u' ', Qt::SkipEmptyParts) };
}

#ifdef Q_OS_LINUX

namespace {

const QString portalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");

// The portal's filters: a(sa(us)), a name and its patterns, each a glob (0)
// or a MIME type (1).
struct PortalRule
{
    uint type;
    QString pattern;
};
using PortalRules = QList<PortalRule>;

struct PortalFilter
{
    QString name;
    PortalRules rules;
};
using PortalFilters = QList<PortalFilter>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalRule &rule)
{
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalRule &rule)
{
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilter &filter)
{
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilter &filter)
{
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

void registerPortalTypes()
{
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalRule>();
        qDBusRegisterMetaType<PortalRules>();
        qDBusRegisterMetaType<PortalFilter>();
        qDBusRegisterMetaType<PortalFilters>();
        return true;
    }();
    Q_UNUSED(registered);
}

// A path as the portal takes one: bytes ending with a zero.
QByteArray portalPath(const QString &path)
{
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}

} // namespace

PortalFileChooser::PortalFileChooser(QObject *parent)
    : QObject(parent)
{
    registerPortalTypes();
}

PortalFileChooser::~PortalFileChooser()
{
    if (m_pending)
        closePending();
}

bool PortalFileChooser::available() const
{
    // Asked once: the portal starts when first called, which takes a moment.
    static const bool available = [] {
        if (qEnvironmentVariableIsSet("COUSOIR_NO_PORTAL") || !QDBusConnection::sessionBus().isConnected())
            return false;
        return QDBusInterface(portalService, QStringLiteral("/org/freedesktop/portal/desktop"),
                              QStringLiteral("org.freedesktop.portal.FileChooser"), QDBusConnection::sessionBus())
            .isValid();
    }();
    return available;
}

void PortalFileChooser::open(Mode mode, const QString &title, const QStringList &nameFilters, const QUrl &folder,
                             const QString &name)
{
    // A request whose answer never came (the portal stopped with its dialog
    // up) must not hold the next one back: close it and take its place.
    if (m_pending) {
        closePending();
        clearPending();
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface portal(portalService, QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.FileChooser"), bus);
    if (!portal.isValid()) {
        emit failed(tr("The desktop's file chooser is not available."));
        return;
    }

    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    if (mode == OpenFiles)
        options.insert(QStringLiteral("multiple"), true);
    if (mode == OpenFolder)
        options.insert(QStringLiteral("directory"), true);
    if (folder.isLocalFile() && !folder.toLocalFile().isEmpty())
        options.insert(QStringLiteral("current_folder"), portalPath(folder.toLocalFile()));
    if (mode == SaveFile && !name.isEmpty())
        options.insert(QStringLiteral("current_name"), name);
    PortalFilters filters;
    for (const QString &nameFilter : nameFilters) {
        const Filter filter = parseFilter(nameFilter);
        PortalRules rules;
        for (const QString &pattern : filter.patterns)
            rules.append({ 0, pattern });
        filters.append({ filter.name, rules });
    }
    if (!filters.isEmpty() && mode != OpenFolder) {
        options.insert(QStringLiteral("filters"), QVariant::fromValue(filters));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(filters.constFirst()));
    }

    // Listen for the answer where the portal will send it, a path made of
    // our name on the bus and this token, before asking: an answer cannot
    // then come before we listen.
    const QString token = QStringLiteral("cousoir_%1").arg(QRandomGenerator::global()->generate());
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = bus.baseService().mid(1);
    sender.replace(u'.', u'_');
    m_pending = true;
    if (!listenTo(QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token))) {
        clearPending();
        emit failed(tr("Could not listen for the file chooser's answer."));
        return;
    }

    const int generation = ++m_generation;
    auto *watcher = new QDBusPendingCallWatcher(
        portal.asyncCall(mode == SaveFile ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"), QString(), title,
                         options),
        this);
    m_watcher = watcher;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_watcher == watcher)
            m_watcher = nullptr;
        // Replaced by a newer request, or answered while the reply was on
        // its way.
        if (generation != m_generation || !m_pending)
            return;
        if (reply.isError()) {
            clearPending();
            emit failed(tr("The file chooser failed: %1").arg(reply.error().message()));
            return;
        }
        // An old portal answers on another path than the token's: listen
        // there instead.
        const QString path = reply.value().path();
        if (path != m_pendingPath) {
            QDBusConnection::sessionBus().disconnect(portalService, m_pendingPath, requestInterface,
                                                     QStringLiteral("Response"), this,
                                                     SLOT(handleResponse(uint,QVariantMap)));
            if (!listenTo(path)) {
                clearPending();
                emit failed(tr("Could not listen for the file chooser's answer."));
            }
        }
    });
}

bool PortalFileChooser::listenTo(const QString &path)
{
    m_pendingPath = path;
    return QDBusConnection::sessionBus().connect(portalService, m_pendingPath, requestInterface, QStringLiteral("Response"),
                                                 this, SLOT(handleResponse(uint,QVariantMap)));
}

void PortalFileChooser::closePending()
{
    if (m_pendingPath.isEmpty())
        return;
    QDBusInterface request(portalService, m_pendingPath, requestInterface, QDBusConnection::sessionBus());
    request.asyncCall(QStringLiteral("Close"));
}

void PortalFileChooser::clearPending()
{
    if (m_watcher) {
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
    if (!m_pendingPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(portalService, m_pendingPath, requestInterface, QStringLiteral("Response"),
                                                 this, SLOT(handleResponse(uint,QVariantMap)));
    }
    m_pendingPath.clear();
    m_pending = false;
}

void PortalFileChooser::handleResponse(uint response, const QVariantMap &results)
{
    clearPending();
    // 1: the user closed the chooser; 2: the portal ended it.
    QList<QUrl> files;
    if (response == 0) {
        const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
        for (const QString &uri : uris)
            files.append(QUrl(uri));
    }
    if (files.isEmpty())
        emit rejected();
    else
        emit accepted(files);
}

#else

PortalFileChooser::PortalFileChooser(QObject *parent)
    : QObject(parent)
{
}

PortalFileChooser::~PortalFileChooser() = default;

bool PortalFileChooser::available() const
{
    return false;
}

void PortalFileChooser::open(Mode, const QString &, const QStringList &, const QUrl &, const QString &)
{
    emit failed(tr("The desktop's file chooser is not available."));
}

bool PortalFileChooser::listenTo(const QString &)
{
    return false;
}

void PortalFileChooser::clearPending()
{
}

void PortalFileChooser::closePending()
{
}

void PortalFileChooser::handleResponse(uint, const QVariantMap &)
{
}

#endif
