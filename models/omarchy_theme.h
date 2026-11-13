#pragma once

#include <QObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

class OmarchyTheme : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap palette READ palette NOTIFY paletteChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // Hyprland's window rounding, which the interface's corners follow; 0
    // without Hyprland.
    Q_PROPERTY(int rounding READ rounding NOTIFY roundingChanged)
    // How see-through Omarchy makes the windows that opt in, as picked under
    // Appearance > Windows > Window Opacity: "opaque", "transparent" (its
    // default) or "glassy".
    Q_PROPERTY(QString windowOpacity READ windowOpacity NOTIFY windowOpacityChanged)

public:
    explicit OmarchyTheme(QObject *parent = nullptr);
    // A test's theme folder, and the hyprctl it asks, none by default.
    explicit OmarchyTheme(const QString &stateRoot, QObject *parent = nullptr, const QString &hyprctl = {});
    ~OmarchyTheme() override;

    QVariantMap palette() const;
    bool available() const;
    bool active() const;
    int rounding() const;
    QString windowOpacity() const;

    // Whether Omarchy is installed for this user, so that windows may be
    // made see-through.
    static bool present();

    void setActive(bool active);

    void reload();

signals:
    void paletteChanged();
    void availableChanged();
    void activeChanged();
    void roundingChanged();
    void windowOpacityChanged();

private:
    void watchRounding(const QString &hyprctl);
    // Hyprland's events, a reload of its configuration among them, instead
    // of asking hyprctl every second.
    void listenToHyprland();
    void refreshRounding();

    QVariantMap m_palette;
    bool m_available = false;
    bool m_active = false;
    QString m_stateRoot;
    QTimer m_refreshTimer;
    QString m_hyprctl;
    int m_rounding = 0;
    QString m_windowOpacity = QStringLiteral("transparent");
    QProcess m_roundingProcess;
    QTimer m_roundingPoll;
    QLocalSocket *m_events = nullptr;
    QByteArray m_eventLine;
    QTimer m_roundingTimeout;
};
