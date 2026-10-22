// SPDX-License-Identifier: GPL-3.0-only
//
// The Python jobs run Calibre's own functions; they follow, Copyright (C)
// Kovid Goyal, GPL v3:
// - src/calibre/gui2/metadata/single_download.py: IdentifyWorker and
//   CoverWorker, which run identify() and run_download() for the dialog
// - src/calibre/ebooks/metadata/sources/worker.py: single_identify() and
//   single_covers(), the jobs the dialog runs, which these jobs call
// - src/calibre/gui2/metadata/single.py: fetch_metadata(), the published
//   date kept as a day, and update_from_mi(), ported in changes()
// - src/calibre/library/comments.py: comments_to_html() and
//   merge_comments(), for a summary added to the book's
// - src/calibre/db/cache.py: author_sort_from_authors()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "metadata_download.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSet>

#include <algorithm>
#include <cmath>

#include "book.h"
#include "book_import.h"
#include "calibre_config.h"
#include "calibre_job.h"
#include "calibre_text.h"

namespace {

// The identify job of the dialog's IdentifyWorker: single_identify(), its
// results read back from their OPF as the dialog reads them, the caches of
// the sources kept for the covers job. The published date is the day it is
// in UTC, which the dialog keeps.
const char identifyScript[] = R"(
import json, os, pickle, sys, traceback
from io import BytesIO

folder = sys.argv[-1]


def path(name):
    return os.path.join(folder, name)


with open(path('job.json'), encoding='utf-8') as f:
    job = json.load(f)
with open(path('log.txt'), 'w', encoding='utf-8') as log_file:
    try:
        from calibre.ebooks.metadata.opf2 import OPF
        from calibre.ebooks.metadata.sources.prefs import msprefs
        from calibre.ebooks.metadata.sources.update import update_sources
        from calibre.ebooks.metadata.sources.worker import single_identify
        from calibre.utils.date import is_date_undefined
        from calibre.utils.logging import GUILog

        update_sources(wait_for_completion=True)
        opfs, has_covers, caches, log_dump = single_identify(job['title'], job['authors'], dict(job['identifiers']))
        log = GUILog()
        log.load(log_dump)
        log_file.write(log.plain_text)
    except Exception:
        log_file.write(traceback.format_exc())
        raise
    books = []
    for opf, has_cover in zip(opfs, has_covers):
        r = OPF(BytesIO(opf), basedir=os.getcwd(), populate_spine=False).to_book_metadata()
        pubdate = r.pubdate
        books.append({
            'title': '' if r.is_null('title') else r.title,
            'authors': [] if r.is_null('authors') else list(r.authors),
            'author_sort': '' if r.is_null('author_sort') else r.author_sort,
            'publisher': r.publisher or '',
            'pubdate': None if pubdate is None or is_date_undefined(pubdate)
                       else '%04d-%02d-%02d' % (pubdate.year, pubdate.month, pubdate.day),
            'series': r.series or '',
            'series_index': None if r.series_index is None else float(r.series_index),
            'tags': list(r.tags or []),
            'rating': float(r.rating) if r.rating else None,
            'comments': r.comments or '',
            'languages': list(r.languages or []),
            'identifiers': sorted(r.get_identifiers().items()),
            'has_cover': bool(has_cover),
        })
    with open(path('caches.pickle'), 'wb') as f:
        pickle.dump(caches, f)
    with open(path('result.json'), 'w', encoding='utf-8') as f:
        json.dump({'books': books, 'ignore_fields': list(msprefs['ignore_fields']),
                   'append_comments': bool(msprefs['append_comments'])}, f)
)";

// The covers job of the dialog's CoverWorker: single_covers(), with the
// caches of the identify job before it, saves each cover in covers/ as it
// comes, beside a folder saying it is whole.
const char coversScript[] = R"(
import json, os, pickle, sys, traceback

folder = sys.argv[-1]


def path(name):
    return os.path.join(folder, name)


with open(path('job.json'), encoding='utf-8') as f:
    job = json.load(f)
with open(path('log.txt'), 'w', encoding='utf-8') as log_file:
    try:
        from calibre.ebooks.metadata.sources.worker import single_covers
        from calibre.utils.logging import GUILog

        caches = {}
        if os.path.exists(path('caches.pickle')):
            with open(path('caches.pickle'), 'rb') as f:
                caches = pickle.load(f)
        os.makedirs(path('covers'), exist_ok=True)
        log_dump = single_covers(job['title'], job['authors'], dict(job['identifiers']), caches, path('covers'))
        log = GUILog()
        log.load(log_dump)
        log_file.write(log.plain_text)
    except Exception:
        log_file.write(traceback.format_exc())
        raise
    with open(path('result.json'), 'w', encoding='utf-8') as f:
        json.dump({'covers': True}, f)
)";

bool prepare(const QString &folder, const char *kind, const char *script, const MetadataDownload::Query &query,
             QString *error)
{
    // The identify job's result goes, as the covers job runs in its folder.
    QFile::remove(QDir(folder).filePath(QStringLiteral("result.json")));
    QJsonArray identifiers;
    for (const auto &[type, value] : query.identifiers)
        identifiers.append(QJsonArray{ type, value });
    const QJsonObject job{
        { QStringLiteral("kind"), QLatin1String(kind) },
        { QStringLiteral("title"), query.title },
        { QStringLiteral("authors"), QJsonArray::fromStringList(query.authors) },
        { QStringLiteral("identifiers"), identifiers },
        { QStringLiteral("timeout"), query.timeout },
    };
    return CalibreJob::writeFile(QDir(folder).filePath(QStringLiteral("job.json")), QJsonDocument(job).toJson(), error)
        && CalibreJob::writeFile(QDir(folder).filePath(QStringLiteral("download.py")), QByteArray(script), error);
}

// The job's result.json, or why there is none.
bool readResult(const QString &folder, QJsonObject *result, QString *error)
{
    QFile file(QDir(folder).filePath(QStringLiteral("result.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        const QString tail = CalibreJob::logTail(folder);
        if (error)
            *error = tail.isEmpty() ? QObject::tr("Calibre stopped before the end of the download.")
                                    : QObject::tr("Calibre stopped before the end of the download: %1").arg(tail);
        return false;
    }
    *result = QJsonDocument::fromJson(file.readAll()).object();
    return true;
}

// comments_to_html() for the texts it leaves as they are or puts in
// paragraphs: HTML, and plain text. Plain text with tags, which Calibre
// cleans up with BeautifulSoup, is put in paragraphs with its tags.
QString commentsToHtml(const QString &comments)
{
    if (comments.isEmpty())
        return QStringLiteral("<p></p>");
    if (comments.trimmed().startsWith(u'<'))
        return comments;
    QString text = comments;
    if (!text.contains(u'<'))
        text.replace(u'&', QStringLiteral("&amp;")).replace(u'<', QStringLiteral("&lt;")).replace(u'>', QStringLiteral("&gt;"));
    QStringList parts;
    for (const QString &part : text.split(QStringLiteral("\n\n")))
        parts.append(QStringLiteral("<p class=\"description\">%1</p>").arg(QString(part).replace(u'\n', QStringLiteral("<br />"))));
    return parts.join(u'\n');
}

QStringList strings(const QJsonValue &value)
{
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list.append(item.toString());
    return list;
}

} // namespace

namespace MetadataDownload {

QString program()
{
    return BookImport::calibreDebug();
}

bool prepareIdentify(const QString &folder, const Query &query, QString *error)
{
    return prepare(folder, "identify", identifyScript, query, error);
}

bool prepareCovers(const QString &folder, const Query &query, QString *error)
{
    return prepare(folder, "covers", coversScript, query, error);
}

QStringList arguments(const QString &folder)
{
    return { QStringLiteral("-e"), QDir(folder).filePath(QStringLiteral("download.py")), QStringLiteral("--"), folder };
}

bool readIdentified(const QString &folder, Identified *identified, QString *error)
{
    QJsonObject result;
    if (!readResult(folder, &result, error))
        return false;
    *identified = {};
    for (const QJsonValue &value : result.value(QLatin1String("books")).toArray()) {
        const QJsonObject book = value.toObject();
        Found found;
        found.title = book.value(QLatin1String("title")).toString();
        found.authors = strings(book.value(QLatin1String("authors")));
        found.authorSort = book.value(QLatin1String("author_sort")).toString();
        found.publisher = book.value(QLatin1String("publisher")).toString();
        const QDate pubdate = QDate::fromString(book.value(QLatin1String("pubdate")).toString(), Qt::ISODate);
        if (pubdate.isValid())
            found.pubdate = pubdate;
        found.series = book.value(QLatin1String("series")).toString();
        if (book.value(QLatin1String("series_index")).isDouble())
            found.seriesIndex = book.value(QLatin1String("series_index")).toDouble();
        found.tags = strings(book.value(QLatin1String("tags")));
        if (book.value(QLatin1String("rating")).isDouble())
            found.rating = book.value(QLatin1String("rating")).toDouble();
        found.comments = book.value(QLatin1String("comments")).toString();
        found.languages = strings(book.value(QLatin1String("languages")));
        for (const QJsonValue &pair : book.value(QLatin1String("identifiers")).toArray())
            found.identifiers.append({ pair.toArray().at(0).toString(), pair.toArray().at(1).toString() });
        found.hasCover = book.value(QLatin1String("has_cover")).toBool();
        identified->books.append(found);
    }
    identified->ignoredFields = strings(result.value(QLatin1String("ignore_fields")));
    identified->appendComments = result.value(QLatin1String("append_comments")).toBool();
    return true;
}

QList<Cover> coversFound(const QString &folder)
{
    // single_covers() names a cover "source,,width,,height,,format.cover",
    // a source with several covers "source{n}", and makes a folder of the
    // same name with ".done" once it is written.
    const QDir covers(QDir(folder).filePath(QStringLiteral("covers")));
    QList<Cover> found;
    for (const QFileInfo &info :
         covers.entryInfoList({ QStringLiteral("*.cover") }, QDir::Files, QDir::Time | QDir::Reversed)) {
        if (!QFileInfo(info.filePath() + QStringLiteral(".done")).isDir())
            continue;
        const QStringList parts = info.completeBaseName().split(QStringLiteral(",,"));
        if (parts.size() != 4)
            continue;
        QString source = parts.at(0);
        if (source.endsWith(u'}'))
            source.truncate(source.lastIndexOf(u'{'));
        found.append({ source, parts.at(1).toInt(), parts.at(2).toInt(), info.filePath() });
    }
    return found;
}

bool readCovers(const QString &folder, QList<Cover> *covers, QString *error)
{
    QJsonObject result;
    if (!readResult(folder, &result, error))
        return false;
    *covers = coversFound(folder);
    // The largest first, as the dialog sorts them once the download is over.
    std::stable_sort(covers->begin(), covers->end(),
                     [](const Cover &a, const Cover &b) { return a.width * a.height > b.width * b.height; });
    return true;
}

const QStringList &fields()
{
    static const QStringList fields{ QStringLiteral("title"),     QStringLiteral("authors"),     QStringLiteral("rating"),
                                     QStringLiteral("publisher"), QStringLiteral("tags"),        QStringLiteral("identifiers"),
                                     QStringLiteral("pubdate"),   QStringLiteral("series"),      QStringLiteral("languages"),
                                     QStringLiteral("comments") };
    return fields;
}

QVariantMap changes(const LibraryData &data, const Book &book, const CalibreConfig &config, const Found &found,
                    const QStringList &fields, bool appendComments)
{
    const QString unknown = CalibreText::unknownText(config.interfaceLanguage);
    const auto use = [&fields](const char *field) { return fields.contains(QLatin1String(field)); };
    QVariantMap changes;
    // update_sorts: the title sort generated again, in the book's language.
    if (use("title") && !found.title.isEmpty() && found.title != unknown) {
        changes.insert(QStringLiteral("title"), found.title);
        changes.insert(QStringLiteral("sort"), CalibreText::titleSort(found.title, book.languages.value(0), config.tweaks,
                                                                      config.interfaceLanguage));
    }
    if (use("authors") && !found.authors.isEmpty() && found.authors != QStringList{ unknown }) {
        changes.insert(QStringLiteral("authors"), found.authors);
        QString sort = found.authorSort == unknown ? QString() : found.authorSort;
        if (sort.isEmpty()) {
            // author_sort_from_authors(): the library's sort of an author it
            // has, Calibre's for another.
            QHash<QString, QString> sorts;
            for (const FacetItem &author : data.authors)
                sorts.insert(author.name.toLower(), author.sort);
            QStringList parts;
            for (const QString &author : found.authors)
                parts.append(sorts.value(author.toLower(), CalibreText::authorToAuthorSort(author, config.tweaks)));
            sort = parts.join(QStringLiteral(" & "));
        }
        changes.insert(QStringLiteral("authorSort"), sort);
    }
    // Out of 10, in whole stars: the dialog's rating editor has no half
    // stars, and cuts a value down to one.
    if (use("rating") && found.rating && *found.rating > 0)
        changes.insert(QStringLiteral("rating"), std::clamp(int(*found.rating * 2), 0, 10) / 2 * 2);
    if (use("publisher") && !found.publisher.isEmpty())
        changes.insert(QStringLiteral("publisher"), found.publisher);
    if (use("tags") && !found.tags.isEmpty()) {
        // merge_tags: the new tags the book lacks, then its own.
        QSet<QString> own;
        for (const QString &tag : book.tags)
            own.insert(tag.toLower());
        QStringList tags;
        for (const QString &tag : found.tags) {
            if (!own.contains(tag.toLower()))
                tags.append(tag);
        }
        changes.insert(QStringLiteral("tags"), tags + book.tags);
    }
    if (use("identifiers") && !found.identifiers.isEmpty()) {
        // The book's, updated with the found ones.
        QVariantMap identifiers;
        for (const auto &[type, value] : book.identifiers)
            identifiers.insert(type, value);
        for (const auto &[type, value] : found.identifiers)
            identifiers.insert(type, value);
        changes.insert(QStringLiteral("identifiers"), identifiers);
    }
    // fetch_metadata(): the day found, at midnight in the local time zone.
    if (use("pubdate") && found.pubdate)
        changes.insert(QStringLiteral("pubdate"), found.pubdate->startOfDay().toOffsetFromUtc(
                                                      found.pubdate->startOfDay().offsetFromUtc()).toString(Qt::ISODate));
    if (use("series") && !found.series.trimmed().isEmpty()) {
        changes.insert(QStringLiteral("series"), found.series);
        if (found.seriesIndex)
            changes.insert(QStringLiteral("seriesIndex"), *found.seriesIndex);
    }
    if (use("languages")) {
        QStringList languages;
        for (const QString &language : found.languages) {
            const QString code = CalibreText::canonicalizeLanguage(language);
            if (!code.isEmpty())
                languages.append(code);
        }
        if (!languages.isEmpty())
            changes.insert(QStringLiteral("languages"), languages);
    }
    if (use("comments") && !found.comments.trimmed().isEmpty()) {
        QString comments = found.comments;
        if (appendComments && !book.comments.isEmpty())
            comments = commentsToHtml(book.comments) + QStringLiteral("\n\n") + commentsToHtml(found.comments);
        changes.insert(QStringLiteral("comments"), comments);
    }
    return changes;
}

} // namespace MetadataDownload
