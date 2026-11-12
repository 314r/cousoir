#include "omarchy_theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QString defaultOmarchyStateRoot()
{
    return QDir::homePath() + QStringLiteral("/.local/state/omarchy/current");
}

QString canonicalPaletteKey(const QString &key)
{
    if (key == QLatin1String("bg"))
        return QStringLiteral("background");
    if (key == QLatin1String("dark_bg"))
        return QStringLiteral("dark_background");
    if (key == QLatin1String("darker_bg"))
        return QStringLiteral("darker_background");
    if (key == QLatin1String("lighter_bg"))
        return QStringLiteral("lighter_background");
    if (key == QLatin1String("fg"))
        return QStringLiteral("foreground");
    if (key == QLatin1String("dark_fg"))
        return QStringLiteral("dark_foreground");
    if (key == QLatin1String("light_fg"))
        return QStringLiteral("light_foreground");
    if (key == QLatin1String("bright_fg"))
        return QStringLiteral("bright_foreground");
    if (key == QLatin1String("selection_background"))
        return QStringLiteral("selection");
    return key;
}

QVariantMap fallbackPalette()
{
    return {
        {QStringLiteral("background"), QColor(QStringLiteral("#030203"))},
        {QStringLiteral("dark_background"), QColor(QStringLiteral("#080708"))},
        {QStringLiteral("darker_background"), QColor(QStringLiteral("#050505"))},
        {QStringLiteral("lighter_background"), QColor(QStringLiteral("#232223"))},
        {QStringLiteral("foreground"), QColor(QStringLiteral("#b5bfc4"))},
        {QStringLiteral("dark_foreground"), QColor(QStringLiteral("#747b7f"))},
        {QStringLiteral("light_foreground"), QColor(QStringLiteral("#a9b2b6"))},
        {QStringLiteral("bright_foreground"), QColor(QStringLiteral("#b3bbbf"))},
        {QStringLiteral("muted"), QColor(QStringLiteral("#5c5c5c"))},
        {QStringLiteral("accent"), QColor(QStringLiteral("#b85d4d"))},
        {QStringLiteral("selection"), QColor(QStringLiteral("#2d2b2d"))},
        {QStringLiteral("red"), QColor(QStringLiteral("#b85d4d"))},
        {QStringLiteral("orange"), QColor(QStringLiteral("#b86f4d"))},
        {QStringLiteral("green"), QColor(QStringLiteral("#687d60"))},
        {QStringLiteral("yellow"), QColor(QStringLiteral("#877364"))},
        {QStringLiteral("cyan"), QColor(QStringLiteral("#b5bfc4"))},
        {QStringLiteral("blue"), QColor(QStringLiteral("#7895a3"))},
        {QStringLiteral("magenta"), QColor(QStringLiteral("#997789"))},
        {QStringLiteral("brown"), QColor(QStringLiteral("#70432e"))},
        {QStringLiteral("bright_red"), QColor(QStringLiteral("#dc7c6a"))},
        {QStringLiteral("bright_yellow"), QColor(QStringLiteral("#a79282"))},
        {QStringLiteral("bright_green"), QColor(QStringLiteral("#8da584"))},
        {QStringLiteral("bright_cyan"), QColor(QStringLiteral("#d6e0e6"))},
        {QStringLiteral("bright_blue"), QColor(QStringLiteral("#9ab7c4"))},
        {QStringLiteral("bright_magenta"), QColor(QStringLiteral("#bd9aaa"))},
    };
}

} // namespace

// Under Hyprland, its own hyprctl: themes and the user's settings can both
// round the windows, so the value in effect is asked for rather than read
// from a configuration file.
OmarchyTheme::OmarchyTheme(QObject *parent)
    : OmarchyTheme(defaultOmarchyStateRoot(), parent,
                   qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                       ? QString() : QStandardPaths::findExecutable(QStringLiteral("hyprctl")))
{
}

OmarchyTheme::OmarchyTheme(const QString &stateRoot, QObject *parent, const QString &hyprctl)
    : QObject(parent)
    , m_stateRoot(QDir::cleanPath(stateRoot))
{
    connect(&m_refreshTimer, &QTimer::timeout, this, &OmarchyTheme::reload);
    m_refreshTimer.setInterval(1500);
    watchRounding(hyprctl);
    reload();
}

OmarchyTheme::~OmarchyTheme()
{
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

// The rounding follows whatever theme Cousoir wears: the windows are
// Hyprland's in every one. As Omarchy's own apps ask for it (Omacut, MIT,
// David Heinemeier Hansson).
void OmarchyTheme::watchRounding(const QString &hyprctl)
{
    m_hyprctl = hyprctl;
    if (m_hyprctl.isEmpty())
        return;
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const QJsonValue value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object()
                                     .value(QLatin1String("int"));
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_rounding) {
            m_rounding = value.toInt();
            emit roundingChanged();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &OmarchyTheme::refreshRounding);
    m_roundingPoll.start();
    refreshRounding();
}

void OmarchyTheme::refreshRounding()
{
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, { QStringLiteral("-j"), QStringLiteral("getoption"),
                                         QStringLiteral("decoration:rounding") });
    m_roundingTimeout.start();
}

QVariantMap OmarchyTheme::palette() const
{
    return m_palette;
}

bool OmarchyTheme::available() const
{
    return m_available;
}

bool OmarchyTheme::active() const
{
    return m_active;
}

int OmarchyTheme::rounding() const
{
    return m_rounding;
}

QString OmarchyTheme::windowOpacity() const
{
    return m_windowOpacity;
}

bool OmarchyTheme::present()
{
    return QFileInfo::exists(defaultOmarchyStateRoot());
}

void OmarchyTheme::setActive(bool active)
{
    if (m_active == active)
        return;

    m_active = active;
    if (m_active) {
        reload();
        m_refreshTimer.start();
    } else {
        m_refreshTimer.stop();
    }
    emit activeChanged();
}

void OmarchyTheme::reload()
{
    QVariantMap palette = fallbackPalette();

    const QString colorsPath = QDir(m_stateRoot).filePath(QStringLiteral("theme/colors.toml"));
    QFile file(colorsPath);
    const bool available = QFileInfo(colorsPath).isReadable();
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            const qsizetype separator = line.indexOf(QLatin1Char('='));
            if (separator < 0)
                continue;

            const QString key = line.left(separator).trimmed();
            QString value = line.mid(separator + 1).trimmed();
            if (value.size() >= 2 && value.startsWith(QLatin1Char('"'))
                && value.endsWith(QLatin1Char('"'))) {
                value = value.mid(1, value.size() - 2);
            }

            const QColor color(value);
            if (key != QLatin1String("mode") && color.isValid())
                palette.insert(canonicalPaletteKey(key), color);
        }
    }

    // The window opacity is a toggle file beside the current theme's folder,
    // as omarchy-hyprland-window-transparency leaves it: opaque or glassy,
    // and transparent without either.
    const QDir toggles(QDir::cleanPath(m_stateRoot + QStringLiteral("/../toggles/hypr")));
    const QString windowOpacity = toggles.exists(QStringLiteral("window-opaque.lua")) ? QStringLiteral("opaque")
        : toggles.exists(QStringLiteral("window-glassy.lua"))                         ? QStringLiteral("glassy")
                                                                                       : QStringLiteral("transparent");
    const bool windowOpacityDidChange = m_windowOpacity != windowOpacity;
    m_windowOpacity = windowOpacity;

    const bool paletteDidChange = m_palette != palette;
    const bool availabilityDidChange = m_available != available;

    m_palette = std::move(palette);
    m_available = available;

    if (paletteDidChange)
        emit paletteChanged();
    if (availabilityDidChange)
        emit availableChanged();
    if (windowOpacityDidChange)
        emit windowOpacityChanged();
}
