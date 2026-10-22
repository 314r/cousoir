#pragma once

#include <QDate>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

#include "metadata_download.h"

class QNetworkAccessManager;
class QNetworkReply;

// A book's metadata and covers from Open Library (openlibrary.org), free and
// without an account: Cousoir's own source, when Calibre is not there to
// ask its own. Its search finds works, each with the edition that best
// matches; the work tells the description and its covers. The results come
// as Calibre's sources give theirs, ranked as Calibre ranks a source's
// results, so the download dialog and its saves are the same.
namespace OpenLibrary {

// The site's addresses, which the tests point at a server of their own.
struct Site
{
    QString api = QStringLiteral("https://openlibrary.org");
    QString covers = QStringLiteral("https://covers.openlibrary.org");
};

// A book found: what the search tells of it, and its work, asked next.
struct Candidate
{
    MetadataDownload::Found found;
    // "/works/OL708597W".
    QString work;
    // Its covers' ids, the edition's first.
    QList<qint64> covers;
};

// Calibre's metadata download preferences that apply to these results,
// from metadata_sources/global.json.
struct Preferences
{
    QStringList ignoredFields;
    bool appendComments = false;
    int maxTags = 20;

    static Preferences load(const QString &calibreConfigDirectory);
};

// get_title_tokens() and get_author_tokens() of Calibre's Source: the
// words of a title and of the first author worth searching for.
QStringList titleTokens(const QString &title);
QStringList authorTokens(const QStringList &authors);
// cleanup_title(): a title as results are compared by.
QString cleanupTitle(QString title);

// The search for the book: by its ISBN, else by title and author; the
// editions in `language`, ISO 639-1, first. Empty when there is nothing to
// look for.
QUrl searchUrl(const Site &site, const MetadataDownload::Query &query, bool byIsbn, const QString &language);
QList<Candidate> parseSearch(const QByteArray &json, const MetadataDownload::Query &query, int maxTags,
                             QString *error);
QUrl workUrl(const Site &site, const QString &work);
// The work's description and covers, added to the candidate.
void addWork(Candidate *candidate, const QByteArray &json);
QUrl coverUrl(const Site &site, qint64 id);

// A publication date as Open Library writes them: "1990-04", "December
// 1999", "May 3, 2001", "2001".
std::optional<QDate> parseDate(const QString &text);

// InternalMetadataCompareKeyGen of Calibre's sources: the results sharing
// an identifier with the book first, then those with a cover, with every
// field, in the interface's language, of the same title, with a longer
// description, and in the order found.
void rank(QList<Candidate> *candidates, const MetadataDownload::Query &query, const QString &interfaceLanguage);

// Asks Open Library over the network: identify(), then covers() of a book
// found. One request of each kind at a time; a new one stops the last.
class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject *parent = nullptr);
    ~Client() override;

    void setSite(const Site &site);
    // `language` is the book's, ISO 639-2, to find its edition first.
    void identify(const MetadataDownload::Query &query, const QString &language, const QString &interfaceLanguage,
                  const Preferences &preferences);
    // Downloads the covers of the book found at `index` into `folder`.
    void covers(int index, const QString &folder);
    void stop();
    void stopCovers();

signals:
    // The books found, the best first, or why none could be.
    void identified(const QList<MetadataDownload::Found> &books, const QString &error);
    void coverFound(const MetadataDownload::Cover &cover);
    // All in; with why some could not be had.
    void coversFinished(const QString &error);

private:
    QNetworkReply *get(const QUrl &url);
    void later(const std::function<void()> &answer);
    void search(bool byIsbn);
    void searched(QNetworkReply *reply, bool byIsbn);
    void finishIdentify();

    QNetworkAccessManager *m_network;
    Site m_site;
    MetadataDownload::Query m_query;
    QString m_language;
    QString m_interfaceLanguage;
    Preferences m_preferences;
    QList<Candidate> m_candidates;
    QList<QPointer<QNetworkReply>> m_identifyReplies;
    QList<QPointer<QNetworkReply>> m_coverReplies;
    int m_worksLeft = 0;
    // Counts the requests stopped, for the answers queued before.
    int m_generation = 0;
    int m_coversLeft = 0;
    QString m_coversError;
};

} // namespace OpenLibrary
