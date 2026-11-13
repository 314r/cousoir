#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

class QDBusPendingCallWatcher;

// Asks for files or a folder through the desktop's portal
// (org.freedesktop.portal.FileChooser), which shows the file chooser the
// desktop picked: Omarchy's own. After Omacut's PortalFilePicker (MIT, David
// Heinemeier Hansson). Without a portal, `available` is false and FilePicker
// shows Qt's dialog instead.
class PortalFileChooser : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    enum Mode { OpenFile, OpenFiles, SaveFile, OpenFolder };
    Q_ENUM(Mode)

    // A filter of Qt's dialogs, "Books (*.epub *.kepub)": its name and its
    // patterns.
    struct Filter
    {
        QString name;
        QStringList patterns;
    };

    explicit PortalFileChooser(QObject *parent = nullptr);
    ~PortalFileChooser() override;

    bool available() const;

    // `folder` is where the chooser opens and `name` the file name it
    // proposes to save under; both may be empty.
    Q_INVOKABLE void open(Mode mode, const QString &title, const QStringList &nameFilters = {},
                          const QUrl &folder = {}, const QString &name = {});

    static Filter parseFilter(const QString &nameFilter);

signals:
    void accepted(const QList<QUrl> &files);
    void rejected();
    void failed(const QString &message);

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    bool listenTo(const QString &path);
    void clearPending();
    void closePending();

    QString m_pendingPath;
    bool m_pending = false;
    // One more for each request: a late reply to a replaced one is ignored.
    int m_generation = 0;
    QDBusPendingCallWatcher *m_watcher = nullptr;
};
