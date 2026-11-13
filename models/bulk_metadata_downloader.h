#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QVariantList>

#include <memory>
#include <optional>

#include "open_library.h"

class BookLibrary;
class MetadataDownloader;
class QTemporaryDir;

// Metadata, and covers, downloaded for many books at once, as Calibre's
// bulk download does it: each book looked up by its title, authors and
// identifiers, a few at a time, its best match taken with the fields
// Calibre's preferences do not leave aside, and the largest cover found.
// The changes wait to be reviewed, each book ticked or not; apply() saves
// those ticked as one change, which Undo puts back.
class BulkMetadataDownloader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    // "idle", "downloading", then "done".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // "calibre" for Calibre's sources, else "openlibrary".
    Q_PROPERTY(QString source READ source NOTIFY stateChanged)
    Q_PROPERTY(bool covers READ covers NOTIFY stateChanged)
    // Each book: { bookId, title, authors, state, error, fields, cover,
    // chosen }; its state "waiting", "searching", "covers", "found",
    // "unchanged" or "failed"; its fields { label, current, found }; its
    // cover found as a file URL, empty for none.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(int done READ done NOTIFY rowsChanged)
    Q_PROPERTY(int total READ total NOTIFY rowsChanged)
    Q_PROPERTY(int chosenCount READ chosenCount NOTIFY rowsChanged)

public:
    explicit BulkMetadataDownloader(QObject *parent = nullptr);
    ~BulkMetadataDownloader() override;

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    QString state() const;
    QString source() const;
    bool covers() const;
    QVariantList rows() const;
    int done() const;
    int total() const;
    int chosenCount() const;

    // Looks up the books, with their covers if `covers`. Returns an error
    // message, empty once the downloads are on their way.
    Q_INVOKABLE QString start(const QVariantList &bookIds, bool covers);
    // Whether the changes found for the book at `row` are saved.
    Q_INVOKABLE void setChosen(int row, bool chosen);
    // Saves the books ticked as one change. Same returns as
    // BookLibrary::updateBook(); the download is over.
    Q_INVOKABLE QString apply();
    // Stops the downloads and forgets them.
    Q_INVOKABLE void cancel();

    // For the tests, as MetadataDownloader's.
    void setProgram(const QString &program);
    void setNative(bool native);
    void setSite(const OpenLibrary::Site &site);

signals:
    void libraryChanged();
    void stateChanged();
    void rowsChanged();

private:
    struct Entry
    {
        int bookId = 0;
        QString title;
        QString authors;
        QString state = QStringLiteral("waiting");
        QString error;
        QVariantList fields;
        QVariantMap changes;
        QString cover;
        bool chosen = false;
    };
    struct Worker
    {
        MetadataDownloader *downloader = nullptr;
        int entry = -1;
        bool coversRunning = false;
    };

    // Each downloader takes the next book waiting, or stops.
    void next(Worker *worker);
    void found(Worker *worker);
    void coversDone(Worker *worker);
    void finish(Worker *worker);
    void setState(const QString &state);

    QPointer<BookLibrary> m_library;
    QString m_program;
    bool m_programSet = false;
    bool m_native = false;
    std::optional<OpenLibrary::Site> m_site;
    QString m_state = QStringLiteral("idle");
    bool m_covers = false;
    QList<Entry> m_entries;
    int m_next = 0;
    int m_done = 0;
    // Counts the stops, for the steps queued before one.
    int m_generation = 0;
    QList<std::shared_ptr<Worker>> m_workers;
    // The covers taken, out of the downloads' own folders.
    std::unique_ptr<QTemporaryDir> m_folder;
};
