// BookLibrary: the queue of changes and their saves.

#include "book_library.h"
#include "book_library_support.h"

using namespace LibrarySupport;

QString BookLibrary::queueChange(const CommitEngine::Request &change)
{
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    m_pending.append(change);
    m_pending.last().id = ++m_lastChangeId;
    emit savingChanged();
    startCommit();
    return {};
}

int BookLibrary::saveDeviceAnnotations(const QList<DeviceAnnotations> &changes, const QList<QPair<int, ReadPosition>> &positions)
{
    CommitEngine::Request request;
    request.deviceAnnotations = changes;
    request.readPositions = positions;
    return queueChange(request).isEmpty() ? m_lastChangeId : -1;
}

int BookLibrary::removeAnnotationUser(const QString &userType, const QString &user)
{
    CommitEngine::Request request;
    request.removedAnnotationUsers.append({ userType, user });
    return queueChange(request).isEmpty() ? m_lastChangeId : -1;
}

void BookLibrary::dropPending(const QString &error)
{
    const QList<CommitEngine::Request> dropped = std::exchange(m_pending, {});
    for (const CommitEngine::Request &request : dropped)
        emit changeFinished(request.id, false, error);
}

CommitEngine::Request BookLibrary::prepared(CommitEngine::Request request) const
{
    request.libraryPath = m_libraryPath;
    request.cacheFolder = cacheFolder(m_libraryPath);
    // One copy of the database before the session's first change.
    request.backupFolder = m_backedUp ? QString() : backupFolder();
    request.config = effectiveConfig();
    const QString version = QCoreApplication::applicationVersion();
    request.producer = version.isEmpty() ? QStringLiteral("Cousoir") : QStringLiteral("Cousoir (%1)").arg(version);
    request.producerId = QStringLiteral("cousoir");
    return request;
}

void BookLibrary::startCommit()
{
    // A load writes the snapshot a save replaces: one at a time.
    if (m_commitWatcher.isRunning() || m_loadWatcher.isRunning() || m_pending.isEmpty())
        return;
    if (!m_writable) {
        dropPending(tr("The library cannot be changed right now."));
        emit savingChanged();
        emit saveFailed(tr("The library cannot be changed right now."));
        return;
    }
    CommitEngine::Request request = prepared(m_pending.takeFirst());
    // The library as shown: the save reads again only what it changes.
    if (m_loaded && m_data.libraryPath == QDir::cleanPath(QFileInfo(m_libraryPath).absoluteFilePath()))
        request.base = m_data;
    const QStringList sortFields = this->sortFields();
    // The index of the library as shown, for a save that reads again only
    // the books it changes.
    const CalibreSearch::Index previous = m_index.search;
    m_commitWatcher.setFuture(QtConcurrent::run([request, sortFields, previous] {
        CommitResult committed{ CommitEngine::commit(request), {}, request.bulkEdits, request.id };
        CommitEngine::Result &result = committed.result;
        if (result.ok && result.written) {
            QElapsedTimer clock;
            clock.start();
            committed.index = indexBooks(result.data, request.config, sortFields,
                                         result.incremental ? &previous : nullptr, result.changed);
            result.timings.append({ QStringLiteral("index the books"), clock.nsecsElapsed() });
            if (request.verify && result.incremental
                && !committed.index.search.sameAs(indexBooks(result.data, request.config, {}).search)) {
                // Saved, but the window would search another library: it
                // reads the library again.
                result.ok = false;
                result.error = QStringLiteral("The search index made again in part differs from the index made whole.");
            }
        }
        return committed;
    }));
    // The files move meanwhile: the check waits for the library saved.
    if (m_checkWatcher.isRunning()) {
        m_checkWatcher.cancel();
        m_checkPending = true;
    }
    emit savingChanged();
}

void BookLibrary::finishCommit()
{
    CommitResult committed = m_commitWatcher.future().takeResult();
    const CommitEngine::Result &result = committed.result;
    if (result.notesWritten && (!result.ok || !result.written)) {
        // The notes went in, whatever came after them.
        CalibreDb::readNotes(&m_data);
        emit filterChanged();
        rebuildFacetRows();
    }
    if (!result.ok) {
        // What follows was made on what failed; the library shows what it
        // holds.
        emit changeFinished(committed.id, false, result.error);
        dropPending(result.error);
        clearBulkUndo();
        dropUndoPlans();
        emit bulkUndoChanged();
        emit saveFailed(result.error);
        m_reloadPending = true;
    } else if (result.data.libraryPath == m_libraryPath) {
        // Not when another library was picked meanwhile: its load, waiting
        // for this save, shows it.
        m_backedUp = m_backedUp || !result.backup.isEmpty();
        // The highlights and notes are read from the snapshot: one that
        // stayed behind is made again by a reload.
        if (result.snapshotStale)
            m_reloadPending = true;
        if (result.written) {
            m_loaded = true;
            m_lastError.clear();
            QElapsedTimer clock;
            clock.start();
            applyData(result.data, std::move(committed.index));
            m_saveTimings = result.timings;
            m_saveTimings.append({ QStringLiteral("show it (the window's thread)"), clock.nsecsElapsed() });
            if (!committed.bulkEdits.isEmpty())
                keepBulkUndo(committed.bulkEdits, result.bulkBefore);
            if (m_undoPlans.contains(committed.id)) {
                const UndoPlan plan = m_undoPlans.take(committed.id);
                keepUndo(plan.fields, plan.before, plan.covers);
            }
            m_publishedKey = ConcurrencyGuard::changeKey(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db")));
            emit publishedKeyChanged();
        }
        emit saved();
        emit changeFinished(committed.id, true, {});
        if (result.written) {
            if (m_textIndexer.running())
                m_textIndexAgain = true;
            else
                startTextIndexing();
        }
        if (!result.added.isEmpty()) {
            QVariantList added;
            for (const int id : result.added)
                added.append(id);
            emit booksAdded(added);
        }
    }
    emit savingChanged();
    if (std::exchange(m_reloadPending, false))
        reloadIfChanged();
    startCommit();
    if (m_checkPending && !m_commitWatcher.isRunning())
        scheduleCheck();
}
