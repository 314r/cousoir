#include "book_text_indexer.h"

#include <QPointer>
#include <QtConcurrent>

BookTextIndexer::BookTextIndexer(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<void>::finished, this, [this] {
        if (m_state)
            m_leftToCalibre.unite(m_state->run.leftToCalibre);
        m_state.reset();
        m_total = 0;
        emit progressChanged();
        emit finished();
    });
}

BookTextIndexer::~BookTextIndexer()
{
    stop();
}

bool BookTextIndexer::running() const
{
    return m_watcher.isRunning();
}

int BookTextIndexer::done() const
{
    return m_state ? m_state->done.load() : 0;
}

int BookTextIndexer::total() const
{
    return m_total;
}

void BookTextIndexer::start(const QString &libraryPath, const QString &interfaceLanguage,
                            QList<BookTextIndex::Task> tasks)
{
    if (m_watcher.isRunning())
        return;
    if (libraryPath != m_libraryPath) {
        m_libraryPath = libraryPath;
        m_leftToCalibre.clear();
    }
    // Formats left to Calibre are not even looked at: they stay waiting.
    tasks.removeIf([this](const BookTextIndex::Task &task) {
        return !task.path.isEmpty() && BookTextIndex::calibreReads(task.format)
               && (!BookTextIndex::reads(task.format) || m_leftToCalibre.contains({ task.book, task.format.toUpper() }));
    });
    if (tasks.isEmpty())
        return;
    auto state = std::make_shared<State>();
    m_state = state;
    m_total = int(tasks.size());
    QPointer<BookTextIndexer> self(this);
    m_watcher.setFuture(QtConcurrent::run([state, tasks, libraryPath, interfaceLanguage, self] {
        state->run = BookTextIndex::index(libraryPath, interfaceLanguage, tasks, &state->cancel, [state, self](int done) {
            state->done = done;
            QMetaObject::invokeMethod(self, [self] {
                if (self)
                    emit self->progressChanged();
            });
        });
    }));
    emit progressChanged();
}

void BookTextIndexer::stop()
{
    if (m_state)
        m_state->cancel = true;
    m_watcher.waitForFinished();
}
