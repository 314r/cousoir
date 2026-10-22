#pragma once

#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "kobo.h"
#include "reading_log.h"

class BookLibrary;

// A mounted volume: its folder and its block device.
struct Volume
{
    QString root;
    QString node;
};

// A Kindle on USB that speaks MTP, by its place on the bus; or, in the
// tests, a folder that stands for one.
struct KindleSlot
{
    quint32 bus = 0;
    quint8 number = 0;
    quint16 productId = 0;
    QString folder;
};

// The readers connected to the computer, each with its books, and the books
// sent to them: Kobos mounted as folders, and Kindles reached over MTP, which
// Cousoir opens for each thing it does there, one at a time, on a thread of
// their own. A reader's books are read once it shows up, and after each
// change Cousoir makes. Sending goes through Calibre as KoboSend and
// KindleSend describe, one batch at a time. While Calibre runs on this
// computer it handles the readers itself, which Cousoir then leaves alone.
class DeviceLibrary : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    // The readers: { root, name, kind, ejectable, free }, with root naming
    // the reader here: the folder a Kobo is mounted on, or "mtp:BUS:NUMBER"
    // for a Kindle; kind "kobo" or "kindle"; free the room left as text.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // The reader the device page shows, by its root.
    Q_PROPERTY(QString currentRoot READ currentRoot WRITE setCurrentRoot NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)
    // Its books, by title: { lpath, title, authors, format, size, bookId,
    // purchased, reading, notes }, with size as text, reading how far it is
    // read, "43 %" or "✓" once finished, and notes the number of its
    // highlights and notes.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY rowsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY rowsChanged)
    // The library's books on each reader: book id → the readers' names.
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
    using KindleProvider = std::function<QList<KindleSlot>()>;
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

    // The format of the book Calibre would send to the reader on `root`,
    // empty for none.
    Q_INVOKABLE QString sendFormat(int bookId, const QString &root) const;
    // What Calibre converts a book to before sending it to the reader on
    // `root`: the first of its driver's formats that Cousoir converts to.
    Q_INVOKABLE QString convertFormat(const QString &root) const;
    // Sends the books to the reader on `root`. Returns an error message,
    // empty once they wait their turn.
    Q_INVOKABLE QString send(const QVariantList &bookIds, const QString &root);
    // Removes books from the current reader, by lpath; a Kobo's bought ones
    // stay.
    Q_INVOKABLE QString remove(const QStringList &lpaths);
    // Unmounts a Kobo and powers it off, so it can be unplugged.
    Q_INVOKABLE QString eject(const QString &root);
    // Stops the batches with the book in them, or all of them for 0: those
    // waiting and Calibre's work on the running one; its copy to the reader,
    // once begun, ends.
    Q_INVOKABLE void cancel(int bookId = 0);
    // Looks for readers again and reads their books.
    Q_INVOKABLE void refresh();
    // What the readers know of the reading of a library book, a reader's
    // copy of it at a time: { name, format, connected, seen, status,
    // percent, state, lastRead, timeSpent, notes, markdown }, with seen,
    // state, lastRead and timeSpent as text, percent -1 when unknown, notes
    // { type, text, annotation, chapter, newChapter, progress, created },
    // created as text and newChapter true for a chapter's first, and
    // markdown the notes as Markdown. A reader unplugged tells what it told
    // when last seen, connected false. It changes with rows.
    Q_INVOKABLE QVariantList reading(int bookId) const;
    // Copies text, notes as Markdown, to the clipboard.
    Q_INVOKABLE void copyNotes(const QString &markdown) const;
    // Saves it to a file; returns an error message, empty on success.
    Q_INVOKABLE QString saveNotes(const QString &markdown, const QUrl &file) const;

    // For the tests: where volumes and Kindles come from and how often to
    // look, the program run in place of calibre-debug, how ejecting is done
    // and Calibre's configuration folder. In QStandardPaths' test mode, there
    // are no volumes or Kindles until a test gives some.
    void setVolumeProvider(VolumeProvider provider, int intervalMs);
    void setKindleProvider(KindleProvider provider);
    void setProgram(const QString &program);
    void setEjector(Ejector ejector);
    void setConfigDirectory(const QString &directory);

signals:
    void libraryChanged();
    void devicesChanged();
    void currentChanged();
    void rowsChanged();
    void availableChanged();
    void writableChanged();
    void jobsChanged();
    // Books arrived on a reader.
    void sent(const QStringList &titles, const QString &deviceName);
    void ejected(const QString &deviceName);
    void failed(const QString &message);

private:
    enum class Kind { Kobo, Kindle };

    struct Connected
    {
        Kind kind = Kind::Kobo;
        QString root;
        QString name;
        qint64 freeBytes = 0;
        Kobo::Device kobo;
        KindleSlot slot;
        // A Kindle's, once read: Calibre keeps its settings by it.
        QString serial;
        QList<DeviceBook> books;
        QString error;
        bool loading = false;
    };

    // A Kindle's MTP connection, from its listing to the end of a send.
    struct KindleLink;

    struct Job
    {
        QList<int> bookIds;
        QStringList titles;
        QString root;
        QString name;
        Kind kind = Kind::Kobo;
        double progress = 0;
        QStringList unsendable;
        std::shared_ptr<KindleLink> link;
    };

    void scan();
    void load(const QString &root);
    // Keeps what a reader tells of the reading of the library's books.
    void recordReading(const Connected &connected);
    QString deviceId(const Connected &connected) const;
    const ReadingLog::Log &readingLog() const;
    void rematch();
    void updateWritable();
    const Connected *find(const QString &root) const;
    Connected *find(const QString &root);
    QStringList formatMap(const Connected &connected) const;
    void startNext();
    void startProcess(const QString &folder);
    void readProgress();
    void finishJob();
    void finishCopy(bool ok, const QString &error);
    void stopProcess();

    QPointer<BookLibrary> m_library;
    VolumeProvider m_volumes;
    KindleProvider m_kindles;
    QTimer m_scanTimer;
    QString m_program;
    Ejector m_ejector;
    QString m_configDirectory;
    QList<Connected> m_devices;
    // What the readers told, for the library at m_readingLogPath.
    mutable ReadingLog::Log m_readingLog;
    mutable QString m_readingLogPath;
    QString m_currentRoot;
    bool m_writable = true;
    QList<Job> m_jobs;
    QProcess *m_process = nullptr;
    std::shared_ptr<QTemporaryDir> m_folder;
    QFutureWatcher<QString> *m_copy = nullptr;
    // Kobos being ejected: they stay out until unplugged.
    QStringList m_ejected;
    // MTP, one thing at a time.
    QThreadPool m_kindlePool;
};
