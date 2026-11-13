#pragma once

#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>

#include <atomic>
#include <memory>

#include "book_text_index.h"

// Indexes the text of a library's books in the background, into Calibre's
// full text index, as Calibre's indexing workers do while its full text
// search is on (BookTextIndex::index()): a save waits for it a moment at
// most. Formats Cousoir cannot read are left to Calibre.
class BookTextIndexer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY progressChanged)
    Q_PROPERTY(int done READ done NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY progressChanged)

public:
    explicit BookTextIndexer(QObject *parent = nullptr);
    ~BookTextIndexer() override;

    bool running() const;
    int done() const;
    int total() const;

    // Indexes `tasks` in `libraryPath`, but those Cousoir found it could not
    // read before; nothing while it already runs.
    void start(const QString &libraryPath, const QString &interfaceLanguage, QList<BookTextIndex::Task> tasks);
    // Stops after the format being read, and waits for it.
    void stop();

signals:
    void progressChanged();
    // Text went in, or the run stopped: the index has changed.
    void finished();

private:
    struct State
    {
        std::atomic_bool cancel = false;
        std::atomic_int done = 0;
        BookTextIndex::Run run;
    };

    QFutureWatcher<void> m_watcher;
    std::shared_ptr<State> m_state;
    int m_total = 0;
    QString m_libraryPath;
    QSet<QPair<int, QString>> m_leftToCalibre;
};
