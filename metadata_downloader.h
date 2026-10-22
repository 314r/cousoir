#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <memory>

#include "metadata_download.h"

class BookLibrary;
class QProcess;
class QTemporaryDir;

// A book's metadata and cover downloaded from the web, as Calibre's Edit
// metadata dialog downloads them: search() asks Calibre's sources for the
// book in a process of its own, which lists the books they found;
// fields() compares one with the book; searchCovers() downloads the covers
// for it; apply() saves the fields chosen and a cover as an edit of the
// book, which Revert undoes while the book is open. One book at a time.
class MetadataDownloader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    // Calibre is there to download.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // "idle", "searching", "found" or "failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int bookId READ bookId NOTIFY stateChanged)
    // Why the search failed.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // The books found, the best first: { title, authors, published,
    // publisher, hasCover, hasSummary }.
    Q_PROPERTY(QVariantList results READ results NOTIFY stateChanged)
    // The book's own cover, then those found as they come, the largest
    // first once all are in: { source, width, height, url, own }.
    Q_PROPERTY(QVariantList covers READ covers NOTIFY coversChanged)
    Q_PROPERTY(bool searchingCovers READ searchingCovers NOTIFY coversChanged)

public:
    explicit MetadataDownloader(QObject *parent = nullptr);
    ~MetadataDownloader() override;

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    bool available() const;
    QString state() const;
    int bookId() const;
    QString error() const;
    QVariantList results() const;
    QVariantList covers() const;
    bool searchingCovers() const;

    // Looks for the book by its title, authors and identifiers. Returns an
    // error message, empty once the search is on its way.
    Q_INVOKABLE QString search(int bookId);
    // What the book found at `result` would change, field by field: { key,
    // label, current, found, chosen }, chosen when it differs and Calibre
    // is not set to leave it aside.
    Q_INVOKABLE QVariantList fields(int result) const;
    // Downloads the covers for the book found at `result`.
    Q_INVOKABLE void searchCovers(int result);
    // Saves `fields` of the book found at `result` and the cover at
    // `cover` of covers, none for the book's own or -1. Same returns as
    // BookLibrary::updateBook(); the download is over.
    Q_INVOKABLE QString apply(int result, const QStringList &fields, int cover);
    // Stops the download and forgets it.
    Q_INVOKABLE void cancel();

    // For the tests: a program run in place of calibre-debug; empty to find
    // Calibre again.
    void setProgram(const QString &program);

signals:
    void libraryChanged();
    void availableChanged();
    void stateChanged();
    void coversChanged();

private:
    void start(bool covers);
    void finishJob();
    void readCovers();
    void stopProcess();
    void setState(const QString &state, const QString &error = {});
    QString coverUrl() const;

    QPointer<BookLibrary> m_library;
    QString m_program;
    QString m_state = QStringLiteral("idle");
    QString m_error;
    int m_bookId = 0;
    MetadataDownload::Query m_query;
    MetadataDownload::Identified m_identified;
    QList<MetadataDownload::Cover> m_covers;
    bool m_coversRunning = false;
    QProcess *m_process = nullptr;
    // The job's folder, the identify job's and then the covers job's.
    std::unique_ptr<QTemporaryDir> m_folder;
    QTimer m_coverPoll;
};
