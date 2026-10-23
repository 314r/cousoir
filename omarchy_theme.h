#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>

class OmarchyTheme : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap palette READ palette NOTIFY paletteChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit OmarchyTheme(QObject *parent = nullptr);
    explicit OmarchyTheme(const QString &stateRoot, QObject *parent = nullptr);

    QVariantMap palette() const;
    bool available() const;
    bool active() const;

    void setActive(bool active);

    void reload();

signals:
    void paletteChanged();
    void availableChanged();
    void activeChanged();

private:
    QVariantMap m_palette;
    bool m_available = false;
    bool m_active = false;
    QString m_stateRoot;
    QTimer m_refreshTimer;
};
