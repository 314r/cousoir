#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

class EpubBook;

// A highlight as Calibre's viewer writes one into the library's
// annotations, and what is written of it there.
namespace HighlightJson {

// The viewer's timestamps: ISO 8601 in UTC, with milliseconds and a Z.
QString isoTimestamp(const QDateTime &time);

// annot_db_data(): the text a search finds a highlight by, its notes after
// a separator, NFKC.
QString searchableText(const QJsonObject &highlight);

// A highlight, `which` one of Calibre's builtin colours: yellow, green,
// blue, red or purple. Its notes and table of contents titles only when
// there are some.
QJsonObject highlight(const QString &uuid, const QString &timestamp, const QString &text, const QString &startCfi,
                      const QString &endCfi, const QString &which, const QString &spineName, int spineIndex,
                      const QStringList &tocFamily, const QString &notes = {});

// remove_highlight(): what is kept of a highlight removed, its id and type.
QJsonObject removed(const QString &uuid, const QString &timestamp);

// The titles of the table of contents entries a place in the spine item
// `spineIndex` is under, as the viewer notes them: those of the items up to
// it, nested by depth. An entry of the item itself with a fragment counts
// only when `before` says the fragment comes before the place; without
// `before`, all do.
QStringList tocFamily(const EpubBook &book, int spineIndex,
                      const std::function<bool(const QString &fragment)> &before = {});

} // namespace HighlightJson
