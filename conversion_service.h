#pragma once

#include <QList>
#include <QObject>
#include <QFutureWatcher>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantList>

#include <memory>

class BookLibrary;
struct Book;

// The conversions asked for in the window, one after the other. Each runs
// Calibre in a process of its own, as Conversion describes, from a folder in
// the library's cache; without Calibre, Cousoir makes a KEPUB of the book's
// EPUB itself (Kepub). The file then goes into the library through the
// library's changes, after those before it. A conversion reads the book as
// it starts, once the changes saved before it are in.
class ConversionService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    // Calibre is there to convert, to every format; without it, Cousoir
    // converts an EPUB to KEPUB itself.
    Q_PROPERTY(bool calibre READ calibre NOTIFY calibreChanged)
    // The conversions, the running one first: { bookId, title, format,
    // running, progress } with progress from 0 to 1.
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY jobsChanged)

public:
    explicit ConversionService(QObject *parent = nullptr);
    ~ConversionService() override;

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    bool calibre() const;
    QVariantList jobs() const;
    bool busy() const;

    // The formats the book can be converted to: Cousoir's output formats
    // when the book has a format Calibre converts from, KEPUB from an EPUB
    // without Calibre; none otherwise.
    Q_INVOKABLE QStringList formatsFor(int bookId) const;
    // Converts the book to `format` after the conversions before it.
    // Returns an error message, empty once the conversion waits its turn.
    Q_INVOKABLE QString convert(int bookId, const QString &format);
    // Stops the book's conversions, or all of them for 0.
    Q_INVOKABLE void cancel(int bookId = 0);

    // For the tests: a program run in place of calibre-debug, with its
    // arguments; empty to find Calibre again.
    void setProgram(const QString &program);
    // For the tests: Cousoir's own KEPUB conversion even with Calibre.
    void setNative(bool native);

signals:
    void libraryChanged();
    void calibreChanged();
    void jobsChanged();
    // A conversion's file is on its way into the library.
    void converted(int bookId, const QString &title, const QString &format);
    void failed(const QString &message);

private:
    struct Job
    {
        int bookId = 0;
        QString title;
        QString format;
        double progress = 0;
    };

    void startNext();
    void readProgress();
    void finishJob();
    void stopProcess();
    void releaseFolders();
    void startNative(const Book &book);
    void finishNative();

    QPointer<BookLibrary> m_library;
    QString m_program;
    // The first one runs while m_process is there.
    QList<Job> m_jobs;
    QProcess *m_process = nullptr;
    bool m_native = false;
    // Cousoir's own conversion, running: its error, empty once done.
    QFutureWatcher<QString> *m_nativeJob = nullptr;
    std::shared_ptr<QTemporaryDir> m_folder;
    // The folders of conversions whose files the library has yet to add.
    QList<std::shared_ptr<QTemporaryDir>> m_handedOver;
};
