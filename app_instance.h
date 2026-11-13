#pragma once

#include <QLocalServer>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

// The one Cousoir of a session. A second launch hands its command line to
// the first, which shows its window, and ends: the same library is never
// opened twice on one computer. One per cache folder, so that a run in the
// sandbox has its own.
class AppInstance : public QObject
{
    Q_OBJECT

public:
    // What a launch asks for.
    struct Invocation
    {
        // Book files to read: those of the library's books are read, the
        // others offered to add.
        QStringList files;
        // Book files to add; `addAsked` alone, none given, opens the picker.
        QStringList add;
        bool addAsked = false;
        // A library to open in this session, as COUSOIR_LIBRARY_PATH does.
        QString library;
        // A book of the library to show.
        int show = 0;

        QVariantMap toMap() const;
        QByteArray encode() const;
        static Invocation decode(const QByteArray &line);
    };

    enum class Forward { Taken, NoInstance, NotAnswering };

    explicit AppInstance(QObject *parent = nullptr);

    // `arguments` as main() has them, the program first. Paths made
    // absolute from the folder the launch was in. False with `error` for
    // options it does not know.
    static bool parse(const QStringList &arguments, Invocation *invocation, QString *error);
    static QString serverName();
    // Hands `invocation` to the Cousoir listening on `name`.
    static Forward forward(const Invocation &invocation, const QString &name = serverName());
    // Listens as the running Cousoir; false when another took the name.
    bool listen(const QString &name = serverName());
    // Asks the compositor to give this process's window the focus, on
    // Hyprland; elsewhere the window's own request is all there is.
    Q_INVOKABLE void focusWindow() const;
    // A notification on the desktop, for work that ends while Cousoir is
    // not in front: a conversion, a book sent.
    Q_INVOKABLE void notify(const QString &summary, const QString &body) const;

signals:
    // A launch's command line, as Invocation::toMap() has it.
    void invoked(const QVariantMap &invocation);

private:
    QLocalServer m_server;
};
