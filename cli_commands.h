#pragma once

// cousoir-cli's commands and what they share, one file a group.

#include <QDir>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "book.h"
#include "calibre_config.h"
#include "commit_engine.h"
#include "kobo_highlights.h"

namespace Cli {

QTextStream &out();
QTextStream &err();
bool load(const QString &libraryPath, LibraryData *data);
QString readOnlyReason(const QString &libraryPath);
QString deviceSpec(const QDir &base, const QString &device);
int commit(const QString &libraryPath, const CalibreConfig &config, CommitEngine::Request &request);
int list(const QString &libraryPath);
int search(const QString &libraryPath, const QString &query, int limit);
int doctor(const QString &libraryPath, bool json);
int indexText(const QString &libraryPath, bool force, bool json);
int exportBooks(const QString &libraryPath, const QString &folder, const QStringList &ids);
int searchText(const QString &libraryPath, const QString &query, int limit, bool json);
int templates(const QString &libraryPath, const QString &file);
int read(const QStringList &paths);
bool send(const QString &libraryPath, const CalibreConfig &config, const QString &spec, const QList<int> &bookIds);
bool removeFromDevice(const QString &spec, const QStringList &lpaths);
int device(const QString &libraryPath, const QString &spec);
bool readKobo(const QString &spec, KoboHighlights::Snapshot *snapshot);
int koboHighlights(const QString &libraryPath, const QString &spec);
bool convert(const QString &libraryPath, const CalibreConfig &config, const QList<QPair<int, QString>> &conversions,
             CommitEngine::Request *request);
int download(const QString &libraryPath, const QString &id, bool covers);
int apply(const QString &libraryPath, const QString &opsPath, bool force);

} // namespace Cli
