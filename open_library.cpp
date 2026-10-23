#include "open_library.h"

// Ported from Calibre 9.16, src/calibre/ebooks/metadata/sources/base.py:
// Source.get_title_tokens(), Source.get_author_tokens(), cleanup_title() and
// InternalMetadataCompareKeyGen; prefs.py: msprefs' defaults. The search
// itself is Open Library's API (https://openlibrary.org/dev/docs/api/search).

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <numeric>

#include "calibre_text.h"
#include "description.h"

namespace OpenLibrary {

namespace {

// The most results asked, and kept.
constexpr int searchLimit = 10;
constexpr int candidateLimit = 5;
// The most covers downloaded for a book.
constexpr int coverLimit = 6;

QStringList strings(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString())
            result.append(item.toString());
    }
    return result;
}

// Open Library's descriptions are Markdown, some with the references of
// their source and a list of what a collection contains after a rule.
QString descriptionHtml(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    static const QRegularExpression rule(QStringLiteral("\\n\\s*-{5,}\\s*(\\n|$)"));
    const QRegularExpressionMatch ruleMatch = rule.match(text);
    if (ruleMatch.hasMatch())
        text.truncate(ruleMatch.capturedStart());
    static const QRegularExpression sourceLink(QStringLiteral("\\(\\[[^\\]]*\\]\\[\\d+\\]\\)"));
    static const QRegularExpression definition(QStringLiteral("(?m)^\\s*\\[\\d+\\]:\\s*\\S+.*$"));
    static const QRegularExpression reference(QStringLiteral("\\[([^\\]]+)\\]\\[\\d+\\]"));
    text.replace(sourceLink, QString());
    text.replace(definition, QString());
    text.replace(reference, QStringLiteral("\\1"));
    return Description::toHtml(text.trimmed());
}

QString capitalized(const QString &text)
{
    return text.isEmpty() ? text : text.at(0).toUpper() + text.mid(1);
}

// The book's language for Open Library, ISO 639-1: "fr" for "fra".
QString shortLanguage(const QString &code)
{
    if (code.size() == 2)
        return code;
    const QLocale::Language language = QLocale::codeToLanguage(code, QLocale::ISO639Part2 | QLocale::ISO639Part3);
    return language == QLocale::AnyLanguage ? QString() : QLocale::languageToCode(language, QLocale::ISO639Part1);
}

// Open Library's language codes are MARC's, ISO 639-2's bibliographic
// ones: "fre" and "ger", which Calibre stores as "fra" and "deu".
QString canonicalLanguage(const QString &code)
{
    const QString canonical = CalibreText::canonicalizeLanguage(code);
    if (!canonical.isEmpty())
        return canonical;
    const QLocale::Language language = QLocale::codeToLanguage(code, QLocale::ISO639Part2B);
    return language == QLocale::AnyLanguage
        ? QString()
        : CalibreText::canonicalizeLanguage(QLocale::languageToCode(language, QLocale::ISO639Part3));
}

QString queryIsbn(const MetadataDownload::Query &query)
{
    for (const auto &[type, value] : query.identifiers) {
        if (type == QLatin1String("isbn"))
            return CalibreText::checkIsbn(value);
    }
    return {};
}

// Whether every field Calibre's Google source touches is there, which
// test_fields() asks of a result.
bool allFields(const MetadataDownload::Found &found)
{
    const bool isbn = std::any_of(found.identifiers.cbegin(), found.identifiers.cend(),
                                  [](const auto &identifier) { return identifier.first == QLatin1String("isbn"); });
    return !found.title.isEmpty() && !found.authors.isEmpty() && !found.tags.isEmpty() && found.pubdate
        && !found.comments.trimmed().isEmpty() && !found.publisher.isEmpty() && isbn && !found.languages.isEmpty();
}

} // namespace

Preferences Preferences::load(const QString &calibreConfigDirectory)
{
    Preferences preferences;
    QFile file(QDir(calibreConfigDirectory).filePath(QStringLiteral("metadata_sources/global.json")));
    if (calibreConfigDirectory.isEmpty() || !file.open(QIODevice::ReadOnly))
        return preferences;
    const QJsonObject prefs = QJsonDocument::fromJson(file.readAll()).object();
    preferences.ignoredFields = strings(prefs.value(QLatin1String("ignore_fields")));
    preferences.appendComments = prefs.value(QLatin1String("append_comments")).toBool(false);
    preferences.maxTags = prefs.value(QLatin1String("max_tags")).toInt(20);
    return preferences;
}

QString cleanupTitle(QString title)
{
    if (title.isEmpty())
        title = QStringLiteral("Unknown");
    static const QRegularExpression prefix(QStringLiteral("^(the|a|an|of|and)\\s+"));
    static const QRegularExpression trailingParenthesis(QStringLiteral("\\(.*\\)$"));
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    title = title.trimmed().toLower();
    title.replace(prefix, QStringLiteral(" "));
    title.replace(trailingParenthesis, QString());
    title.replace(whitespace, QStringLiteral(" "));
    return title.trimmed();
}

QStringList titleTokens(const QString &title)
{
    if (title.isEmpty())
        return {};
    // Calibre's patterns, in order. The last one, meant to turn punctuation
    // into spaces, also wants "《》「」“”" after the character, so it changes
    // nothing in practice: kept as Calibre has it.
    static const QList<QPair<QRegularExpression, QString>> patterns{
        { QRegularExpression(QStringLiteral("[({\\[](\\d{4}|omnibus|anthology|hardcover|audiobook|audio\\scd|paperback|"
                                            "turtleback|mass\\s*market|edition|ed\\.)[\\])}]"),
                             QRegularExpression::CaseInsensitiveOption),
          QString() },
        { QRegularExpression(QStringLiteral("[({\\[].*?(edition|ed.).*?[\\]})]"), QRegularExpression::CaseInsensitiveOption),
          QString() },
        { QRegularExpression(QStringLiteral("(\\d+),(\\d+)"), QRegularExpression::CaseInsensitiveOption),
          QStringLiteral("\\1\\2") },
        { QRegularExpression(QStringLiteral("(\\s-)"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(" ") },
        { QRegularExpression(QStringLiteral("[:,;!@$%^&*(){}.`~\"\\s\\[\\]/]《》「」“”"),
                             QRegularExpression::CaseInsensitiveOption),
          QStringLiteral(" ") },
    };
    QString text = title;
    for (const auto &[pattern, replacement] : patterns)
        text.replace(pattern, replacement);
    QStringList tokens;
    for (QString token : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        token = token.trimmed();
        while (token.startsWith(u'"') || token.endsWith(u'"')) {
            token = token.startsWith(u'"') ? token.mid(1) : token.chopped(1);
        }
        while (token.startsWith(u'\'') || token.endsWith(u'\'')) {
            token = token.startsWith(u'\'') ? token.mid(1) : token.chopped(1);
        }
        const QString lower = token.toLower();
        if (!token.isEmpty() && lower != QLatin1String("a") && lower != QLatin1String("and")
            && lower != QLatin1String("the") && lower != QLatin1String("&"))
            tokens.append(token);
    }
    return tokens;
}

QStringList authorTokens(const QStringList &authors)
{
    if (authors.isEmpty())
        return {};
    // Leaves ' for Irish names.
    static const QRegularExpression remove(QStringLiteral("[!@#$%^&*()（）「」{}`~\"\\s\\[\\]/]"));
    static const QRegularExpression replace(QStringLiteral("[-+.:;,，。；：]"));
    QString author = authors.first();
    const bool comma = author.contains(u',');
    author.replace(replace, QStringLiteral(" "));
    QStringList parts = author.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    // "Last, First" most likely.
    if (comma && !parts.isEmpty())
        parts.append(parts.takeFirst());
    QStringList tokens;
    for (QString token : std::as_const(parts)) {
        token = token.remove(remove).trimmed();
        const QString lower = token.toLower();
        if (token.size() > 2 && lower != QLatin1String("von") && lower != QLatin1String("van")
            && lower != QLatin1String("unknown"))
            tokens.append(token);
    }
    return tokens;
}

QUrl searchUrl(const Site &site, const MetadataDownload::Query &query, bool byIsbn, const QString &language)
{
    QUrlQuery parameters;
    if (byIsbn) {
        const QString isbn = queryIsbn(query);
        if (isbn.isEmpty())
            return {};
        parameters.addQueryItem(QStringLiteral("isbn"), isbn);
    } else {
        const QStringList title = titleTokens(query.title);
        const QStringList author = authorTokens(query.authors);
        if (title.isEmpty() && author.isEmpty())
            return {};
        if (!title.isEmpty())
            parameters.addQueryItem(QStringLiteral("title"), title.join(u' '));
        if (!author.isEmpty())
            parameters.addQueryItem(QStringLiteral("author"), author.join(u' '));
    }
    parameters.addQueryItem(QStringLiteral("fields"),
                            QStringLiteral("key,title,author_name,first_publish_year,cover_i,subject,editions,"
                                           "editions.key,editions.title,editions.language,editions.publisher,"
                                           "editions.publish_date,editions.isbn,editions.cover_i"));
    parameters.addQueryItem(QStringLiteral("limit"), QString::number(searchLimit));
    const QString lang = shortLanguage(language);
    if (!lang.isEmpty())
        parameters.addQueryItem(QStringLiteral("lang"), lang);
    QUrl url(site.api + QStringLiteral("/search.json"));
    url.setQuery(parameters);
    return url;
}

QList<Candidate> parseSearch(const QByteArray &json, const MetadataDownload::Query &query, int maxTags, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject()) {
        if (error)
            *error = QCoreApplication::translate("OpenLibrary", "Open Library answered something unexpected: %1")
                         .arg(parseError.errorString());
        return {};
    }
    const QString wantedIsbn = queryIsbn(query);
    QList<Candidate> candidates;
    for (const QJsonValue &value : document.object().value(QLatin1String("docs")).toArray()) {
        const QJsonObject work = value.toObject();
        const QString workTitle = work.value(QLatin1String("title")).toString().trimmed();
        // Works merged into another, left behind as their duplicates.
        if (workTitle.isEmpty() || workTitle.contains(QLatin1String("(Duplicate of")))
            continue;
        const QJsonObject edition =
            work.value(QLatin1String("editions")).toObject().value(QLatin1String("docs")).toArray().first().toObject();
        Candidate candidate;
        candidate.work = work.value(QLatin1String("key")).toString();
        MetadataDownload::Found &found = candidate.found;

        // The work's title, better cased and accented, unless the edition's
        // is another, a translation's for instance.
        const QString editionTitle = edition.value(QLatin1String("title")).toString().trimmed();
        found.title = editionTitle.isEmpty()
                || CalibreText::foldForSearch(editionTitle) == CalibreText::foldForSearch(workTitle)
            ? workTitle
            : editionTitle;
        found.authors = strings(work.value(QLatin1String("author_name")));
        found.publisher = strings(edition.value(QLatin1String("publisher"))).value(0);
        const QString published = strings(edition.value(QLatin1String("publish_date"))).value(0);
        found.pubdate = parseDate(published);
        if (!found.pubdate && work.contains(QLatin1String("first_publish_year")))
            found.pubdate = parseDate(QString::number(work.value(QLatin1String("first_publish_year")).toInt()));
        for (const QString &code : strings(edition.value(QLatin1String("language")))) {
            const QString language = canonicalLanguage(code);
            if (!language.isEmpty() && !found.languages.contains(language))
                found.languages.append(language);
        }

        // Subjects: "series:" ones name the series, others are tags, split
        // where Open Library joins a heading and its parts.
        QStringList seen;
        for (const QString &subject : strings(work.value(QLatin1String("subject")))) {
            if (subject.startsWith(QLatin1String("series:"))) {
                if (found.series.isEmpty())
                    found.series = subject.mid(7).trimmed();
                continue;
            }
            if (subject.contains(u':'))
                continue;
            for (const QString &part : subject.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
                const QString tag = capitalized(part.trimmed());
                if (tag.isEmpty() || tag.compare(QLatin1String("general"), Qt::CaseInsensitive) == 0
                    || seen.contains(tag.toLower()))
                    continue;
                seen.append(tag.toLower());
                found.tags.append(tag);
            }
        }
        found.tags = found.tags.mid(0, std::max(0, maxTags));

        QStringList isbns;
        for (const QString &raw : strings(edition.value(QLatin1String("isbn")))) {
            const QString isbn = CalibreText::checkIsbn(raw);
            if (!isbn.isEmpty() && !isbns.contains(isbn))
                isbns.append(isbn);
        }
        QString isbn = isbns.contains(wantedIsbn) ? wantedIsbn : QString();
        for (const QString &candidateIsbn : std::as_const(isbns)) {
            if (isbn.isEmpty() && candidateIsbn.size() == 13)
                isbn = candidateIsbn;
        }
        if (isbn.isEmpty())
            isbn = isbns.value(0);
        if (!isbn.isEmpty())
            found.identifiers.append({ QStringLiteral("isbn"), isbn });

        for (const QJsonValue &cover : { edition.value(QLatin1String("cover_i")), work.value(QLatin1String("cover_i")) }) {
            const qint64 id = cover.toInteger(0);
            if (id > 0 && !candidate.covers.contains(id))
                candidate.covers.append(id);
        }
        found.hasCover = !candidate.covers.isEmpty();
        candidates.append(candidate);
        if (candidates.size() == candidateLimit)
            break;
    }
    return candidates;
}

QUrl workUrl(const Site &site, const QString &work)
{
    return QUrl(site.api + work + QStringLiteral(".json"));
}

void addWork(Candidate *candidate, const QByteArray &json)
{
    const QJsonObject work = QJsonDocument::fromJson(json).object();
    const QJsonValue description = work.value(QLatin1String("description"));
    const QString text = description.isObject() ? description.toObject().value(QLatin1String("value")).toString()
                                                : description.toString();
    if (!text.trimmed().isEmpty())
        candidate->found.comments = descriptionHtml(text);
    for (const QJsonValue &cover : work.value(QLatin1String("covers")).toArray()) {
        // -1 stands for a cover deleted.
        const qint64 id = cover.toInteger(0);
        if (id > 0 && !candidate->covers.contains(id))
            candidate->covers.append(id);
    }
    candidate->found.hasCover = !candidate->covers.isEmpty();
}

QUrl coverUrl(const Site &site, qint64 id)
{
    // Not found rather than a blank image when there is none.
    return QUrl(site.covers + QStringLiteral("/b/id/%1-L.jpg?default=false").arg(id));
}

std::optional<QDate> parseDate(const QString &raw)
{
    // parse_only_date() of Calibre's utils/date.py, as its sources read a
    // date: what is missing taken from the 15th of this month, then
    // fix_only_date(), so that the day stays the same in every time zone.
    const QDate today = QDateTime::currentDateTimeUtc().date();
    const auto fixed = [](int year, int month, int day) -> std::optional<QDate> {
        QDate date(year, month, day);
        if (!date.isValid())
            date = QDate(year, month, 1).addMonths(1).addDays(-1);
        if (!date.isValid())
            return std::nullopt;
        if (date.addDays(1).month() > date.month())
            date = date.addDays(-1);
        if (date.day() == 1)
            date = date.addDays(1);
        return date;
    };
    const QString text = raw.trimmed();
    static const QRegularExpression iso(QStringLiteral("^(\\d{4})(?:-(\\d{1,2})(?:-(\\d{1,2}))?)?$"));
    if (const QRegularExpressionMatch match = iso.match(text); match.hasMatch()) {
        return fixed(match.captured(1).toInt(), match.hasCaptured(2) ? match.captured(2).toInt() : today.month(),
                     match.hasCaptured(3) ? match.captured(3).toInt() : 15);
    }
    static const QStringList months{ QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                                     QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"),
                                     QStringLiteral("jul"), QStringLiteral("aug"), QStringLiteral("sep"),
                                     QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec") };
    static const QRegularExpression year(QStringLiteral("\\b(\\d{4})\\b"));
    const QRegularExpressionMatch yearMatch = year.match(text);
    if (!yearMatch.hasMatch())
        return std::nullopt;
    int month = 0;
    int day = 0;
    static const QRegularExpression word(QStringLiteral("[A-Za-z]+"));
    for (auto it = word.globalMatch(text); it.hasNext();) {
        const int index = int(months.indexOf(it.next().captured(0).left(3).toLower()));
        if (index >= 0) {
            month = index + 1;
            break;
        }
    }
    static const QRegularExpression dayNumber(QStringLiteral("\\b(\\d{1,2})(st|nd|rd|th)?\\b"));
    if (month > 0) {
        if (const QRegularExpressionMatch match = dayNumber.match(text); match.hasMatch())
            day = match.captured(1).toInt();
    }
    return fixed(yearMatch.captured(1).toInt(), month > 0 ? month : today.month(), day > 0 ? day : 15);
}

void rank(QList<Candidate> *candidates, const MetadataDownload::Query &query, const QString &interfaceLanguage)
{
    struct Key
    {
        std::array<int, 5> base;
        qsizetype comments;
        int relevance;
    };
    const QString userLanguage = CalibreText::canonicalizeLanguage(interfaceLanguage);
    QList<Key> keys;
    for (int i = 0; i < candidates->size(); ++i) {
        const MetadataDownload::Found &found = candidates->at(i).found;
        int sameIdentifier = 2;
        for (const auto &[type, value] : query.identifiers) {
            for (const auto &[foundType, foundValue] : found.identifiers) {
                if (type == foundType && value == foundValue)
                    sameIdentifier = 1;
            }
        }
        // A cover's url from Open Library is as reliable as its search.
        const int hasCover = found.hasCover ? 1 : 2;
        const int fields = allFields(found) ? 1 : 2;
        int language = 1;
        if (!found.languages.isEmpty()) {
            const QString code = found.languages.first();
            if (code != QLatin1String("und") && code != userLanguage)
                language = 2;
        }
        const int exactTitle = !query.title.isEmpty() && cleanupTitle(query.title) == cleanupTitle(found.title) ? 1 : 2;
        keys.append({ { sameIdentifier, hasCover, fields, language, exactTitle },
                      found.comments.trimmed().size(),
                      i });
    }
    // compare_to_other(): a description over 10 % longer counts after the
    // rest, then the order found.
    const auto compare = [](const Key &x, const Key &y) {
        if (x.base != y.base)
            return x.base < y.base ? -1 : 1;
        if (x.comments && y.comments) {
            const double threshold = double(x.comments + y.comments) / 20;
            const qsizetype delta = y.comments - x.comments;
            if (std::abs(double(delta)) > threshold)
                return delta < 0 ? -1 : 1;
        }
        return x.relevance < y.relevance ? -1 : (x.relevance > y.relevance ? 1 : 0);
    };
    QList<int> order(candidates->size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return compare(keys.at(a), keys.at(b)) < 0; });
    QList<Candidate> sorted;
    for (const int index : std::as_const(order))
        sorted.append(candidates->at(index));
    *candidates = sorted;
}

Client::Client(QObject *parent) : QObject(parent), m_network(new QNetworkAccessManager(this)) {}

Client::~Client()
{
    stop();
}

void Client::setSite(const Site &site)
{
    m_site = site;
}

QNetworkReply *Client::get(const QUrl &url)
{
    QNetworkRequest request(url);
    // Open Library asks applications to say who they are.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Cousoir/%1").arg(QCoreApplication::applicationVersion()));
    request.setTransferTimeout(std::max(1, m_query.timeout) * 1000);
    return m_network->get(request);
}

void Client::identify(const MetadataDownload::Query &query, const QString &language, const QString &interfaceLanguage,
                      const Preferences &preferences)
{
    stop();
    m_query = query;
    m_language = language;
    m_interfaceLanguage = interfaceLanguage;
    m_preferences = preferences;
    m_candidates.clear();
    search(!queryIsbn(query).isEmpty());
}

void Client::search(bool byIsbn)
{
    const QUrl url = searchUrl(m_site, m_query, byIsbn, m_language);
    if (url.isEmpty()) {
        // Queued, as an answer would come: a caller waits for it once the
        // call returns.
        later([this] { emit identified({}, tr("The book has no title, author or ISBN to look for.")); });
        return;
    }
    QNetworkReply *reply = get(url);
    m_identifyReplies.append(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, byIsbn] { searched(reply, byIsbn); });
}

void Client::searched(QNetworkReply *reply, bool byIsbn)
{
    reply->deleteLater();
    m_identifyReplies.removeAll(reply);
    if (reply->error() != QNetworkReply::NoError) {
        emit identified({}, tr("Open Library cannot be reached: %1").arg(reply->errorString()));
        return;
    }
    QString error;
    m_candidates = parseSearch(reply->readAll(), m_query, std::min(m_preferences.maxTags, 20), &error);
    if (!error.isEmpty()) {
        emit identified({}, error);
        return;
    }
    // Nothing by its ISBN: by its title and authors, as Calibre's Google
    // source falls back to a search.
    if (m_candidates.isEmpty() && byIsbn && (!m_query.title.isEmpty() || !m_query.authors.isEmpty())) {
        search(false);
        return;
    }
    m_worksLeft = int(m_candidates.size());
    if (m_worksLeft == 0) {
        finishIdentify();
        return;
    }
    for (int i = 0; i < m_candidates.size(); ++i) {
        QNetworkReply *work = get(workUrl(m_site, m_candidates.at(i).work));
        m_identifyReplies.append(work);
        connect(work, &QNetworkReply::finished, this, [this, work, i] {
            work->deleteLater();
            m_identifyReplies.removeAll(work);
            // Without its work, a book keeps what the search told.
            if (work->error() == QNetworkReply::NoError)
                addWork(&m_candidates[i], work->readAll());
            if (--m_worksLeft == 0)
                finishIdentify();
        });
    }
}

void Client::finishIdentify()
{
    rank(&m_candidates, m_query, m_interfaceLanguage);
    QList<MetadataDownload::Found> books;
    for (const Candidate &candidate : std::as_const(m_candidates))
        books.append(candidate.found);
    emit identified(books, {});
}

void Client::covers(int index, const QString &folder)
{
    stopCovers();
    m_coversError.clear();
    const QList<qint64> ids = m_candidates.value(index).covers.mid(0, coverLimit);
    m_coversLeft = int(ids.size());
    if (m_coversLeft == 0) {
        later([this] { emit coversFinished({}); });
        return;
    }
    for (const qint64 id : ids) {
        QNetworkReply *reply = get(coverUrl(m_site, id));
        m_coverReplies.append(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, id, folder] {
            reply->deleteLater();
            m_coverReplies.removeAll(reply);
            if (reply->error() == QNetworkReply::NoError) {
                const QString file = QDir(folder).filePath(QStringLiteral("openlibrary-%1.jpg").arg(id));
                QSaveFile out(file);
                const QByteArray bytes = reply->readAll();
                if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit()) {
                    const QSize size = QImageReader(file).size();
                    if (size.isValid())
                        emit coverFound({ QStringLiteral("Open Library"), size.width(), size.height(), file });
                    else
                        QFile::remove(file);
                }
            } else if (reply->error() != QNetworkReply::ContentNotFoundError) {
                m_coversError = tr("Some covers could not be downloaded: %1").arg(reply->errorString());
            }
            if (--m_coversLeft == 0)
                emit coversFinished(m_coversError);
        });
    }
}

void Client::later(const std::function<void()> &answer)
{
    // Dropped if the request is stopped meanwhile.
    QMetaObject::invokeMethod(
        this,
        [this, answer, generation = m_generation] {
            if (generation == m_generation)
                answer();
        },
        Qt::QueuedConnection);
}

void Client::stop()
{
    ++m_generation;
    for (const QPointer<QNetworkReply> &reply : std::exchange(m_identifyReplies, {})) {
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }
    stopCovers();
}

void Client::stopCovers()
{
    ++m_generation;
    for (const QPointer<QNetworkReply> &reply : std::exchange(m_coverReplies, {})) {
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }
    m_coversLeft = 0;
}

} // namespace OpenLibrary
