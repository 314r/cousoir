// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/metadata/sources/identify.py: urls_from_identifiers()
// - src/calibre/ebooks/metadata/sources/google.py, amazon.py and
//   edelweiss.py: get_book_url() and get_book_url_name() of the metadata
//   sources Calibre has, in their order
// - src/calibre/ebooks/metadata/__init__.py: check_issn()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "identifier_links.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

namespace IdentifierLinks {

namespace {

// Amazon's get_domain_and_asin(): the first identifier of a domain it knows,
// "asin" for amazon.com.
std::optional<Link> amazon(const QList<QPair<QString, QString>> &identifiers)
{
    static const QStringList domains{ QStringLiteral("com"), QStringLiteral("fr"), QStringLiteral("de"),
                                      QStringLiteral("uk"),  QStringLiteral("au"), QStringLiteral("it"),
                                      QStringLiteral("jp"),  QStringLiteral("es"), QStringLiteral("br"),
                                      QStringLiteral("in"),  QStringLiteral("nl"), QStringLiteral("cn"),
                                      QStringLiteral("ca"),  QStringLiteral("se") };
    static const QHash<QString, QString> hosts{ { QStringLiteral("uk"), QStringLiteral("www.amazon.co.uk") },
                                                { QStringLiteral("au"), QStringLiteral("www.amazon.com.au") },
                                                { QStringLiteral("br"), QStringLiteral("www.amazon.com.br") },
                                                { QStringLiteral("jp"), QStringLiteral("www.amazon.co.jp") } };
    for (const auto &[type, value] : identifiers) {
        QString domain;
        if (type == QLatin1String("amazon") || type == QLatin1String("asin"))
            domain = QStringLiteral("com");
        else if (type.startsWith(QLatin1String("amazon_")) && domains.contains(type.mid(7)))
            domain = type.mid(7);
        else
            continue;
        if (value.isEmpty())
            return std::nullopt;
        const QString host = hosts.value(domain, QStringLiteral("www.amazon.") + domain);
        const QString idType = domain == QLatin1String("com") ? QStringLiteral("amazon") : QStringLiteral("amazon_") + domain;
        const QString name = domain == QLatin1String("com") ? QStringLiteral("Amazon.com") : QStringLiteral("Amazon.") + domain;
        return Link{ name, idType, value, QStringLiteral("https://%1/dp/%2").arg(host, value) };
    }
    return std::nullopt;
}

// urlparse(url).netloc or .path.
QString urlName(const QString &url)
{
    QString rest = url.section(u':', 1);
    if (rest.startsWith(QLatin1String("//"))) {
        rest = rest.mid(2);
        const qsizetype end = rest.indexOf(QRegularExpression(QStringLiteral("[/?#]")));
        const QString netloc = end < 0 ? rest : rest.left(end);
        if (!netloc.isEmpty())
            return netloc;
        rest = end < 0 ? QString() : rest.mid(end);
    }
    const qsizetype end = rest.indexOf(QRegularExpression(QStringLiteral("[?#]")));
    return end < 0 ? rest : rest.left(end);
}

// An identifier that is a URL, its ':' and ',' written '|' as Calibre
// stores them: the URL, for http, https and file ones.
std::optional<QString> asUrl(const QString &value)
{
    if (value.isEmpty())
        return std::nullopt;
    const QString url = value.left(8).replace(u'|', u':') + value.mid(8).replace(u'|', u',');
    const QString scheme = url.section(u':', 0, 0).toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("file"))
        return url;
    return std::nullopt;
}

} // namespace

Rules readRules(const QString &configDirectory)
{
    QFile file(QDir(configDirectory).filePath(QStringLiteral("metadata_sources/global.json")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    Rules rules;
    const QJsonObject all = QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("id_link_rules")).toObject();
    for (auto it = all.begin(); it != all.end(); ++it) {
        for (const QJsonValue &rule : it.value().toArray()) {
            const QJsonArray pair = rule.toArray();
            if (pair.size() == 2)
                rules[it.key()].append({ pair.at(0).toString(), pair.at(1).toString() });
        }
    }
    return rules;
}

QString checkIssn(const QString &issn)
{
    QString digits;
    for (const QChar c : issn.toUpper()) {
        if ((c >= u'0' && c <= u'9') || c == u'X')
            digits.append(c);
    }
    if (digits.size() < 8)
        return {};
    int sum = 0;
    for (int i = 0; i < 7; ++i) {
        if (digits.at(i) == u'X')
            return {};
        sum += (8 - i) * digits.at(i).digitValue();
    }
    const int check = 11 - sum % 11;
    const QChar last = digits.at(7);
    if ((check == 10 && last == u'X') || (last != u'X' && check == last.digitValue()))
        return digits;
    return {};
}

QList<Link> links(const QList<QPair<QString, QString>> &given, const Rules &rules, const RuleFormatter &format)
{
    // By lower case type, the last of a type kept, in the order given.
    QList<QPair<QString, QString>> identifiers;
    for (const auto &[type, value] : given) {
        const QString key = type.toLower();
        const auto existing = std::find_if(identifiers.begin(), identifiers.end(),
                                           [&key](const auto &identifier) { return identifier.first == key; });
        if (existing != identifiers.end())
            existing->second = value;
        else
            identifiers.append({ key, value });
    }
    const auto get = [&identifiers](const char *type) {
        for (const auto &[key, value] : identifiers) {
            if (key == QLatin1String(type))
                return value;
        }
        return QString();
    };
    QList<Link> result;
    QSet<QString> left;
    for (const auto &identifier : std::as_const(identifiers))
        left.insert(identifier.first);
    const auto add = [&result, &left](const Link &link) {
        result.append(link);
        left.remove(link.type);
    };

    if (format) {
        for (const auto &[type, raw] : std::as_const(identifiers)) {
            const QString value = QString(raw).replace(u'|', u',');
            const QHash<QString, QString> values{
                { QStringLiteral("id"), QString::fromLatin1(QUrl::toPercentEncoding(value, "/")) },
                { QStringLiteral("id_unquoted"), value },
            };
            for (const auto &[name, text] : rules.value(type)) {
                if (const std::optional<QString> url = format(text, values))
                    add({ name, type, value, *url });
            }
        }
    }
    // The metadata sources, in Calibre's order: Google, Amazon, Edelweiss.
    const QString google = get("google");
    for (const auto &identifier : std::as_const(identifiers)) {
        if (identifier.first == QLatin1String("google")) {
            add({ QStringLiteral("Google"), QStringLiteral("google"), google,
                  QStringLiteral("https://books.google.com/books?id=") + google });
            break;
        }
    }
    if (const std::optional<Link> link = amazon(identifiers))
        add(*link);
    if (const QString sku = get("edelweiss"); !sku.isEmpty())
        add({ QStringLiteral("Edelweiss"), QStringLiteral("edelweiss"), sku,
              QStringLiteral("https://www.edelweiss.plus/#sku=%1&page=1").arg(sku) });

    if (const QString isbn = get("isbn"); !isbn.isEmpty())
        add({ isbn, QStringLiteral("isbn"), isbn, QStringLiteral("https://www.worldcat.org/isbn/") + isbn });
    if (const QString doi = get("doi"); !doi.isEmpty())
        add({ QStringLiteral("DOI"), QStringLiteral("doi"), doi, QStringLiteral("https://dx.doi.org/") + doi });
    if (const QString arxiv = get("arxiv"); !arxiv.isEmpty())
        add({ QStringLiteral("arXiv"), QStringLiteral("arxiv"), arxiv, QStringLiteral("https://arxiv.org/abs/") + arxiv });
    if (const QString oclc = get("oclc"); !oclc.isEmpty())
        add({ QStringLiteral("OCLC"), QStringLiteral("oclc"), oclc, QStringLiteral("https://www.worldcat.org/oclc/") + oclc });
    if (const QString issn = checkIssn(get("issn")); !issn.isEmpty())
        add({ issn, QStringLiteral("issn"), issn, QStringLiteral("https://www.worldcat.org/issn/") + issn });

    static const QRegularExpression urlType(QStringLiteral("^ur[il]\\d*$"));
    for (const auto &[type, value] : std::as_const(identifiers)) {
        if (!urlType.match(type).hasMatch())
            continue;
        if (const std::optional<QString> url = asUrl(value))
            add({ urlName(*url), type, *url, *url });
    }
    for (const auto &[type, value] : std::as_const(identifiers)) {
        if (!left.contains(type))
            continue;
        if (const std::optional<QString> url = asUrl(value))
            add({ urlName(*url), type, *url, *url });
    }
    return result;
}

} // namespace IdentifierLinks
