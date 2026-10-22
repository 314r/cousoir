#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

#include <functional>
#include <optional>

// The links Calibre makes of a book's identifiers, as its book details show
// them and its urls_from_identifiers() template function writes them: the
// links of its metadata sources, then ISBN, DOI, arXiv, OCLC and ISSN ones,
// and the identifiers that are themselves URLs.
namespace IdentifierLinks {

struct Link
{
    // What the link says: "Google", "Amazon.fr", the ISBN…
    QString name;
    QString type;
    QString value;
    QString url;
};

// msprefs['id_link_rules'] of Calibre's preferences: by identifier type,
// the links the user added, [name, template] pairs.
using Rules = QHash<QString, QList<QPair<QString, QString>>>;
Rules readRules(const QString &configDirectory);

// urls_from_identifiers(), unsorted. `format` makes the URL of a rule from
// its template with the fields "id", the value quoted for a URL, and
// "id_unquoted"; nothing when the template fails.
using RuleFormatter = std::function<std::optional<QString>(const QString &, const QHash<QString, QString> &)>;
QList<Link> links(const QList<QPair<QString, QString>> &identifiers, const Rules &rules = {},
                  const RuleFormatter &format = {});

// check_issn(): the ISSN's digits when its check digit is right, empty
// otherwise.
QString checkIssn(const QString &issn);

} // namespace IdentifierLinks
