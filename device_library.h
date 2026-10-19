#pragma once

#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "kobo.h"

class BookLibrary;

// A mounted volume: its folder and its block device.
struct Volume
{
    QString root;
    QString node;
};

// The Kobos connected to the computer, each with its books, and the books
// sent to them. A Kobo shows up while mounted; its books are read once then,
// and after each change Reliure makes. Sending goes through Calibre as
// KoboSend describes, one batch at a time; removing and ejecting happen
// there and then. While Calibre runs on this computer it handles the Kobo
// itself, which Reliure then leaves alone.
class DeviceLibrary : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    // The Kobos: { root, name, freeBytes, free, totalBytes, firmware }, with
    // free the room left as text.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // The Kobo the device page shows, by its root.
    Q_PROPERTY(QString currentRoot READ currentRoot WRITE setCurrentRoot NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)
    // Its books, by title: { lpath, title, authors, format, size, bookId,
    // purchased, imported }, with size as text.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY rowsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY rowsChanged)
    // The library's books on each Kobo: book id → the Kobos' names.
    Q_PROPERTY(QVariantMap onDevice READ onDevice NOTIFY rowsChanged)
    // Calibre is there to send with.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool writable READ writable NOTIFY writableChanged)
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY writableChanged)
    // The batches to send, the running one first: { bookIds, titles, name,
    // root, running, progress } with progress from 0 to 1.
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY jobsChanged)

public:
    using VolumeProvider = std::function<QList<Volume>()>;
    // Ejects the device and calls `done` with an error, empty on success.
    using Ejector = std::function<void(const Kobo::Device &, std::function<void(const QString &)> done)>;

    explicit DeviceLibrary(QObject *parent = nullptr);
    ~DeviceLibrary() override;

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    QVariantList devices() const;
    QString currentRoot() const;
    void setCurrentRoot(const QString &root);
    QVariantMap current() const;
    QVariantList rows() const;
    bool loading() const;
    QString lastError() const;
    QVariantMap onDevice() const;
    bool available() const;
    bool writable() const;
    QString readOnlyReason() const;
    QVariantList jobs() const;
    bool busy() const;

    // The format of the book Calibre would send, empty for none.
    Q_INVOKABLE QString sendFormat(int bookId) const;
    // Sends the books to the Kobo on `root`. Returns an error message, empty
    // once they wait their turn.
    Q_INVOKABLE QString send(const QVariantList &bookIds, const QString &root);
    // Removes books from the current Kobo, by lpath; bought ones stay.
    Q_INVOKABLE QString remove(const QStringList &lpaths);
    // Unmounts the Kobo and powers it off, so it can be unplugged.
    Q_INVOKABLE QString eject(const QString &root);
    // Stops the batches with the book in them, or all of them for 0: those
    // waiting and Calibre's work on the running one; its copy to the Kobo,
    // once begun, ends.
    Q_INVOKABLE void cancel(int bookId = 0);
    // Looks for Kobos again and reads their books.
    Q_INVOKABLE void refresh();

    // For the tests: where volumes come from and how often to look, the
    // program run in place of calibre-debug, how ejecting is done, Linux's
    // /sys and Calibre's configuration folder.
    void setVolumeProvider(VolumeProvider provider, int intervalMs);
    void setProgram(const QString &program);
    void setEjector(Ejector ejector);
    void setSysRoot(const QString &sysRoot);
    void setConfigDirectory(const QString &directory);

signals:
    void libraryChanged();
    void devicesChanged();
    void currentChanged();
    void rowsChanged();
    void availableChanged();
    void writableChanged();
    void jobsChanged();
    // Books arrived on a Kobo.
    void sent(const QStringList &titles, const QString &deviceName);
    void ejected(const QString &deviceName);
    void failed(const QString &message);

private:
    struct Job
    {
        QList<int> bookIds;
        QStringList titles;
        QString root;
        QString name;
        double progress = 0;
        bool copying = false;
        QStringList unsendable;
    };

    struct Connected
    {
        Kobo::Device device;
        QList<Kobo::DeviceBook> books;
        QString error;
        bool loading = false;
    };

    void scan();
    void load(const QString &root);
    void rematch();
    void updateWritable();
    const Connected *find(const QString &root) const;
    Connected *find(const QString &root);
    void startNext();
    void readProgress();
    void finishJob();
    void finishCopy(bool ok, const QString &error);
    void stopProcess();

    QPointer<BookLibrary> m_library;
    VolumeProvider m_volumes;
    QTimer m_scanTimer;
    QString m_program;
    Ejector m_ejector;
    QString m_sysRoot = QStringLiteral("/sys");
    QString m_configDirectory;
    QList<Connected> m_devices;
    QString m_currentRoot;
    bool m_writable = true;
    QList<Job> m_jobs;
    QProcess *m_process = nullptr;
    std::shared_ptr<QTemporaryDir> m_folder;
    QFutureWatcher<QString> *m_copy = nullptr;
    // Kobos being ejected: they stay out until unplugged.
    QStringList m_ejected;
};
