#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <optional>
#include <utility>

class QJsonObject;
class QRegularExpression;

// What the commands of cousoir-test-tool share.
namespace Tool {

QTextStream &out();
QTextStream &err();

// The files under `root` by their path relative to it, '/' between
// folders, as os.walk() finds them: hidden ones too, without going into
// linked folders.
QMap<QString, QString> files(const QString &root);

std::optional<QByteArray> readFile(const QString &path);
bool writeFile(const QString &path, const QByteArray &data);
std::optional<QJsonValue> readJson(const QString &path);
QString sha256(const QByteArray &data);

// A zip archive's entries, in their order.
using ZipEntries = QList<std::pair<QString, QByteArray>>;
std::optional<ZipEntries> readZip(const QString &path);
// Writes the entries in their order, deflated but those in `stored`, all
// with one time: what Python's zipfile writes with ZipInfo(name, stamp).
bool writeZip(const QString &path, const ZipEntries &entries, const QStringList &stored);

// re.sub() on bytes: the pattern sees each byte as one Latin-1 character,
// so \b, \d, \s and \w are ASCII's, as in a pattern of bytes.
QByteArray substitute(const QByteArray &data, const QRegularExpression &pattern, const QString &replacement);

// A key's value, null when it is missing, as dict.get() gives it.
QJsonValue get(const QJsonObject &object, const QString &key);
// A JSON value in one line, for messages.
QString display(const QJsonValue &value);
// The extension of a file's name with its dot, as os.path.splitext() splits
// it: none for a name that only starts with a dot.
QString extension(const QString &path);

// The commands, each given its arguments; they return the exit code.

// compare.cpp
int compareLibraries(const QStringList &arguments);
int compareDevices(const QStringList &arguments);
int compareReads(const QStringList &arguments);
int compareReading(const QStringList &arguments);
int compareTemplates(const QStringList &arguments);

// readers.cpp
int koboImport(const QStringList &arguments);
int kindleSync(const QStringList &arguments);
int runSql(const QStringList &arguments);
int deviceLpath(const QStringList &arguments);
int deviceBooks(const QStringList &arguments);
int checkMatches(const QStringList &arguments);
// Run as calibre-debug runs a sending job: the job in `folder`, done the
// fake way.
int fakeSend(const QString &folder);

// library.cpp
int conversionSettings(const QStringList &arguments);
int pickConversions(const QStringList &arguments);
int pickSends(const QStringList &arguments);
int jsonGet(const QStringList &arguments);

// fixtures.cpp
int makeEpub(const QStringList &arguments);
int makeImportEpubs(const QStringList &arguments);
int makePdf(const QStringList &arguments);

// large.cpp
int makeLibrary(const QStringList &arguments);

// search.cpp
int searchBooks(const QStringList &arguments);
int compareSearches(const QStringList &arguments);

// sort.cpp
int sortBooks(const QStringList &arguments);
int compareSorts(const QStringList &arguments);

} // namespace Tool
