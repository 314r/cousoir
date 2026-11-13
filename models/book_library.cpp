// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/utils.py: find_identical_books()
// - src/calibre/ebooks/__init__.py: BOOK_EXTENSIONS
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "book_library.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextDocument>
#include <QUrl>
#include <QPromise>
#include <QSet>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <numeric>

#include "book_import.h"
#include "book_text_index.h"
#include "new_library.h"
#include "book_text_search.h"
#ifdef COUSOIR_READER
#include "book_reader.h"
#endif
#include "calibre_db.h"
#include "calibre_text.h"
#include "concurrency_guard.h"
#include "conversion.h"
#include "cover_cache.h"
#include "field_writers.h"
#include "cousoir_paths.h"
#include "description.h"
#include "palette.h"
#include "sqlite_db.h"

#include "book_library_support.h"

using namespace LibrarySupport;


BookLibrary::BookLibrary(QObject *parent)
    : BookLibrary(QString(), CalibreConfig::load(), parent)
{
}

BookLibrary::BookLibrary(const QString &libraryPath, const CalibreConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    m_defaultPath = libraryPath.isEmpty();
    m_libraryPath = m_defaultPath ? CalibreConfig::defaultLibraryPath()
                                  : QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath());
    m_collator = listCollator();
    m_changeTimer.setSingleShot(true);
    m_changeTimer.setInterval(1500);
    connect(&m_changeTimer, &QTimer::timeout, this, &BookLibrary::reloadIfChanged);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_changeTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_changeTimer, qOverload<>(&QTimer::start));
    connect(&m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &BookLibrary::finishLoad);
    connect(&m_commitWatcher, &QFutureWatcher<CommitResult>::finished, this, &BookLibrary::finishCommit);
    connect(&m_saveToDiskWatcher, &QFutureWatcher<SaveToDisk::Outcome>::finished, this, [this] {
        const SaveToDisk::Outcome outcome = m_saveToDiskWatcher.future().takeResult();
        emit savedToDisk(outcome.saved, outcome.failures, m_saveToDiskFolder);
    });
    connect(&m_textIndexer, &BookTextIndexer::finished, this, [this] {
        emit textIndexChanged();
        if (std::exchange(m_textIndexAgain, false))
            startTextIndexing();
    });
    connect(&m_importWatcher, &QFutureWatcher<ImportResult>::finished, this, &BookLibrary::finishImport);
    connect(&m_importWatcher, &QFutureWatcher<ImportResult>::progressValueChanged, this, &BookLibrary::importChanged);
    connect(&m_checkWatcher, &QFutureWatcher<LibraryCheck::Findings>::finished, this, &BookLibrary::finishCheck);
    connect(this, &BookLibrary::libraryChanged, this, &BookLibrary::countsChanged);
    connect(this, &BookLibrary::trashChanged, this, &BookLibrary::countsChanged);
    // QML may still set another library; only the last one loads.
    QMetaObject::invokeMethod(this, &BookLibrary::startLoad, Qt::QueuedConnection);
}

BookLibrary::~BookLibrary()
{
    m_textIndexer.stop();
    m_saveToDiskWatcher.waitForFinished();
    m_checkWatcher.cancel();
    m_checkWatcher.waitForFinished();
    m_loadWatcher.waitForFinished();
    // The changes still waiting their turn are saved before the library
    // goes, so that quitting loses none: unless the one being saved failed,
    // what follows having been made on it.
    bool saved = m_writable;
    const QFuture<CommitResult> running = m_commitWatcher.future();
    if (running.isValid() && !running.isCanceled()) {
        m_commitWatcher.waitForFinished();
        // Not taken by finishCommit(): the save ended with no one to hear.
        if (running.isValid() && running.resultCount() > 0)
            saved = saved && running.result().result.ok;
    }
    while (saved && !m_pending.isEmpty()) {
        const CommitEngine::Result result = CommitEngine::commit(prepared(m_pending.takeFirst()));
        saved = result.ok;
        m_backedUp = m_backedUp || !result.backup.isEmpty();
    }
    m_importWatcher.waitForFinished();
}

QString BookLibrary::cacheFolder(const QString &libraryPath)
{
    return CousoirPaths::cacheFolder(libraryPath);
}

QString BookLibrary::libraryPath() const
{
    return m_libraryPath;
}

void BookLibrary::setLibraryPath(const QString &path)
{
    const bool useDefault = path.trimmed().isEmpty();
    const QString cleaned = useDefault ? CalibreConfig::defaultLibraryPath()
                                       : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    m_defaultPath = useDefault;
    if (m_libraryPath == cleaned)
        return;
    m_libraryPath = cleaned;
    emit libraryPathChanged();
    m_textIndexer.stop();
    m_data = {};
    m_index = {};
    m_sorted.clear();
    m_sortedBy.clear();
    m_restriction.clear();
    m_searchedText.reset();
    m_sectionRows.clear();
    m_sectionError.clear();
    m_visibleItems.clear();
    m_loaded = false;
    m_filterKind.clear();
    m_filterId = 0;
    m_currentBookId = 0;
    m_checkWatcher.cancel();
    m_findings = {};
    m_checkedAt = {};
    // The changes waiting are the other library's, as was its backup.
    dropPending(tr("Another library is open."));
    m_importedBooks.clear();
    m_importedDuplicates.clear();
    m_backedUp = false;
    clearBulkUndo();
    dropUndoPlans();
    // An undo lasts a session: covers kept for one before are stale.
    QDir(undoCoverFolder()).removeRecursively();
    emit bulkUndoChanged();
    rebuildCheck();
    emit libraryChanged();
    emit filterChanged();
    emit savingChanged();
    rebuildRows();
    rebuildFacetRows();
    rebuildCurrentBook();
    startLoad();
}

bool BookLibrary::loaded() const
{
    return m_loaded;
}

bool BookLibrary::loading() const
{
    return m_loadWatcher.isRunning();
}

QString BookLibrary::lastError() const
{
    return m_lastError;
}

QString BookLibrary::libraryUuid() const
{
    return m_data.uuid;
}

int BookLibrary::schemaVersion() const
{
    return m_data.userVersion;
}

bool BookLibrary::schemaKnown() const
{
    return m_loaded && m_data.schemaKnown;
}

QVariantMap BookLibrary::counts() const
{
    const bool restricted = !m_virtualLibrary.isEmpty() && m_virtualLibraryError.isEmpty();
    const auto items = [&](const char *kind) {
        return restricted ? visibleItems(QLatin1String(kind)).size() : facetItems(QLatin1String(kind))->size();
    };
    return {
        { QStringLiteral("books"), restricted ? m_restriction.count(true) : m_data.books.size() },
        { QStringLiteral("authors"), items("authors") },
        { QStringLiteral("series"), items("series") },
        { QStringLiteral("tags"), items("tags") },
        { QStringLiteral("languages"), items("languages") },
        { QStringLiteral("publishers"), items("publishers") },
        // Nothing for an empty trash.
        { QStringLiteral("trash"), m_trashEntries.isEmpty() ? QVariant(QString()) : QVariant(m_trashEntries.size()) },
        { QStringLiteral("highlights"), m_data.annotationCount ? QVariant(m_data.annotationCount) : QVariant(QString()) },
    };
}
