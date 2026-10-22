// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/utils/search_query_parser.py: Parser, SearchQueryParser
// - src/calibre/db/search.py: _matchkind(), _match(), DateSearch,
//   NumericSearch, KeyPairSearch, Parser.get_matches(),
//   get_user_category_matches(), Search._do_search()
// - src/calibre/db/fields.py: the values each field searches
// - src/calibre/library/field_metadata.py: the fields' search terms and kinds
//
// The table it includes, calibre_search.inc, is made by
// tools/oracle/search_oracle.py from Calibre's own ICU collators.
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "calibre_search.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "calibre_db.h"

namespace CalibreSearch {

namespace {

struct SearchFold
{
    char32_t code;
    const char *ignoringPunctuation;
    const char *keepingPunctuation;
};

struct SearchWords
{
    const char *language;
    // today, yesterday, thismonth, daysago, No, no, Yes, yes, unchecked,
    // checked, empty and blank.
    const char *words[12];
};

#include "calibre_search.inc"

QString tr(const char *text)
{
    return QCoreApplication::translate("CalibreSearch", text);
}

// The fold of every character of the Basic Multilingual Plane, by code
// unit: 0 for its simple lower case, else one more than its entry.
struct FoldTable
{
    struct Entry
    {
        QByteArray folds[2];
        // The places inside each fold where one of its characters ends.
        QList<int> inside[2];
    };
    std::vector<quint16> index;
    std::vector<Entry> entries;
};

const FoldTable &foldTable()
{
    static const FoldTable table = [] {
        FoldTable result;
        result.index.assign(0x10000, 0);
        for (const SearchFold &fold : searchFolds) {
            if (fold.code >= 0x10000)
                continue;
            FoldTable::Entry entry;
            const char *texts[2] = { fold.ignoringPunctuation, fold.keepingPunctuation };
            for (int keep = 0; keep < 2; ++keep) {
                entry.folds[keep] = QByteArray(texts[keep]);
                const QString characters = QString::fromUtf8(entry.folds[keep]);
                int offset = 0;
                for (qsizetype i = 0; i + 1 < characters.size(); ++i) {
                    offset += QStringView(characters).mid(i, 1).toUtf8().size();
                    if (!characters.at(i).isHighSurrogate())
                        entry.inside[keep].append(offset);
                }
            }
            result.entries.push_back(entry);
            result.index[fold.code] = quint16(result.entries.size());
        }
        return result;
    }();
    return table;
}

void appendUtf8(QByteArray &out, char32_t code)
{
    if (code < 0x80) {
        out.append(char(code));
    } else if (code < 0x800) {
        out.append(char(0xC0 | (code >> 6)));
        out.append(char(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.append(char(0xE0 | (code >> 12)));
        out.append(char(0x80 | ((code >> 6) & 0x3F)));
        out.append(char(0x80 | (code & 0x3F)));
    } else {
        out.append(char(0xF0 | (code >> 18)));
        out.append(char(0x80 | ((code >> 12) & 0x3F)));
        out.append(char(0x80 | ((code >> 6) & 0x3F)));
        out.append(char(0x80 | (code & 0x3F)));
    }
}

// icu_lower(): the full lower case.
QString lower(const QString &text)
{
    return text.toLower();
}

// Python's str.strip() is empty.
bool blank(const QString &text)
{
    return std::all_of(text.cbegin(), text.cend(), [](QChar c) { return CalibreText::isSpace(c); });
}

// The parse tree: and, or, not, and the terms, a location and a query.
struct Node
{
    enum Kind { And, Or, Not, Term };
    Kind kind = Term;
    std::shared_ptr<const Node> left;
    std::shared_ptr<const Node> right;
    QString location;
    QString query;
};
using Tree = std::shared_ptr<const Node>;

// Parser of search_query_parser.py: the tokens, then
//
//   or_expression ::= and_expression [ 'or' or_expression ]
//   and_expression ::= not_expression [ [ 'and' ] and_expression ]
//   not_expression ::= [ 'not' ] location_expression
//   location_expression ::= base_token | ( '(' or_expression ')' )
class Parser
{
public:
    explicit Parser(const QSet<QString> &locations) : m_locations(locations) {}

    Tree parse(const QString &expression, QString *error)
    {
        tokenize(expression);
        m_current = 0;
        m_error.clear();
        Tree tree = orExpression();
        if (m_error.isEmpty() && !atEnd())
            m_error = tr("Extra characters at end of search");
        if (!m_error.isEmpty()) {
            *error = m_error;
            return {};
        }
        return tree;
    }

private:
    enum class Type { Opcode, Word, QuotedWord, End };

    struct Token
    {
        Type type;
        QString text;
    };

    // \\, \", \( and \) stand for themselves; a """docstring""" holds
    // anything. They are put aside as characters no query has.
    static constexpr char16_t escapes[4] = { u'\\', u'"', u'(', u')' };

    void tokenize(QString expression)
    {
        m_tokens.clear();
        QStringList docstrings;
        static const QRegularExpression docstring(QStringLiteral("(\"\"\")(..*?)(\"\"\")"),
                                                  QRegularExpression::DotMatchesEverythingOption);
        QString replaced;
        qsizetype last = 0;
        QRegularExpressionMatchIterator it = docstring.globalMatch(expression);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            replaced += QStringView(expression).mid(last, match.capturedStart() - last);
            replaced += QChar(0xF8F0) + QString::number(docstrings.size()) + QChar(0xF8F1);
            docstrings.append(match.captured(2));
            last = match.capturedEnd();
        }
        replaced += QStringView(expression).mid(last);
        expression = replaced;
        for (int i = 0; i < 4; ++i)
            expression.replace(QString(u'\\') + QChar(escapes[i]), QString(QChar(char16_t(i + 1))));

        const auto unescape = [&docstrings](QString text) {
            static const QRegularExpression placeholder(QStringLiteral("\\x{F8F0}(\\d+)\\x{F8F1}"));
            QString result;
            qsizetype from = 0;
            QRegularExpressionMatchIterator matches = placeholder.globalMatch(text);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                result += QStringView(text).mid(from, match.capturedStart() - from);
                result += docstrings.value(match.captured(1).toInt());
                from = match.capturedEnd();
            }
            result += QStringView(text).mid(from);
            for (int i = 0; i < 4; ++i)
                result.replace(QChar(char16_t(i + 1)), QChar(escapes[i]));
            return result;
        };

        // re.Scanner: at each place the first pattern that matches; where
        // none does, the rest is left out.
        const qsizetype size = expression.size();
        const auto special = [](QChar c) { return c == u'"' || c == u'(' || c == u')' || CalibreText::isSpace(c); };
        qsizetype i = 0;
        while (i < size) {
            const QChar c = expression.at(i);
            if (c == u'(' || c == u')') {
                m_tokens.append({ Type::Opcode, QString(c) });
                ++i;
                continue;
            }
            // @.+?:[^")\s]+, a user category with spaces in its name.
            if (c == u'@') {
                qsizetype end = -1;
                for (qsizetype colon = expression.indexOf(u':', i + 2); colon >= 0 && end < 0;
                     colon = expression.indexOf(u':', colon + 1)) {
                    qsizetype j = colon + 1;
                    while (j < size && expression.at(j) != u'"' && expression.at(j) != u')'
                           && !CalibreText::isSpace(expression.at(j)))
                        ++j;
                    if (j > colon + 1)
                        end = j;
                }
                if (end > 0) {
                    m_tokens.append({ Type::Word, unescape(expression.mid(i, end - i)) });
                    i = end;
                    continue;
                }
            }
            if (!special(c)) {
                qsizetype j = i;
                while (j < size && !special(expression.at(j)))
                    ++j;
                m_tokens.append({ Type::Word, unescape(expression.mid(i, j - i)) });
                i = j;
                continue;
            }
            if (c == u'"') {
                qsizetype close = expression.indexOf(u'"', i + 1);
                while (close >= 0 && expression.at(close - 1) == u'\\')
                    close = expression.indexOf(u'"', close + 1);
                if (close < 0)
                    break;
                m_tokens.append({ Type::QuotedWord, unescape(expression.mid(i + 1, close - i - 1)) });
                i = close + 1;
                continue;
            }
            if (CalibreText::isSpace(c)) {
                ++i;
                continue;
            }
            break;
        }
    }

    bool atEnd() const { return m_current >= m_tokens.size(); }
    Type type() const { return atEnd() ? Type::End : m_tokens.at(m_current).type; }
    QString token() const { return atEnd() ? QString() : m_tokens.at(m_current).text; }
    // Of any kind of token, quoted ones included, as Calibre reads them.
    QString lowerToken() const { return lower(token()); }

    Tree make(Node::Kind kind, Tree left, Tree right = {})
    {
        auto node = std::make_shared<Node>();
        node->kind = kind;
        node->left = std::move(left);
        node->right = std::move(right);
        return node;
    }

    Tree term(const QString &location, const QString &query)
    {
        auto node = std::make_shared<Node>();
        node->location = location;
        node->query = query;
        return node;
    }

    Tree orExpression()
    {
        Tree left = andExpression();
        if (!m_error.isEmpty())
            return {};
        if (!atEnd() && lowerToken() == QLatin1String("or")) {
            ++m_current;
            return make(Node::Or, left, orExpression());
        }
        return left;
    }

    Tree andExpression()
    {
        Tree left = notExpression();
        if (!m_error.isEmpty())
            return {};
        if (!atEnd() && lowerToken() == QLatin1String("and")) {
            ++m_current;
            return make(Node::And, left, andExpression());
        }
        // The "and" left out.
        if ((type() == Type::Word || type() == Type::QuotedWord || (type() == Type::Opcode && token() == u"("))
            && lowerToken() != QLatin1String("or"))
            return make(Node::And, left, andExpression());
        return left;
    }

    Tree notExpression()
    {
        if (!atEnd() && lowerToken() == QLatin1String("not")) {
            ++m_current;
            return make(Node::Not, notExpression());
        }
        return locationExpression();
    }

    Tree locationExpression()
    {
        if (type() == Type::Opcode && token() == u"(") {
            ++m_current;
            Tree result = orExpression();
            if (!m_error.isEmpty())
                return {};
            if (type() != Type::Opcode || token() != u")") {
                m_error = tr("missing )");
                return {};
            }
            ++m_current;
            return result;
        }
        if (type() != Type::Word && type() != Type::QuotedWord) {
            m_error = tr("Invalid syntax. Expected a lookup name or a word");
            return {};
        }
        return baseToken();
    }

    // A location only when the text before the first colon is one: else the
    // colons belong to the query, "all:foo:bar".
    Tree baseToken()
    {
        if (type() == Type::QuotedWord)
            return term(QStringLiteral("all"), m_tokens.at(m_current++).text);
        QStringList words = m_tokens.at(m_current++).text.split(u':');
        if (words.size() > 1 && m_locations.contains(lower(words.first()))) {
            const QString location = lower(words.takeFirst());
            // location:"quoted words"
            if (words.size() == 1 && type() == Type::QuotedWord)
                return term(location, m_tokens.at(m_current++).text);
            return term(location, words.join(u':'));
        }
        return term(QStringLiteral("all"), words.join(u':'));
    }

    const QSet<QString> &m_locations;
    QList<Token> m_tokens;
    qsizetype m_current = 0;
    QString m_error;
};

// The fields: their key, datatype and search terms, as field_metadata.py
// has them.
enum class DataType { None, Text, Series, Rating, Int, Float, DateTime };

struct Field
{
    const char *key;
    DataType type;
    // Several values per book, which `#` counts.
    bool multiple;
    std::initializer_list<const char *> terms;
};

const Field fields[] = {
    { "authors", DataType::Text, true, { "authors", "author" } },
    { "languages", DataType::Text, true, { "languages", "language" } },
    { "series", DataType::Series, false, { "series" } },
    { "formats", DataType::Text, true, { "formats", "format" } },
    { "publisher", DataType::Text, false, { "publisher" } },
    { "rating", DataType::Rating, false, { "rating" } },
    { "tags", DataType::Text, true, { "tags", "tag" } },
    { "identifiers", DataType::Text, true, { "identifiers", "identifier", "isbn" } },
    { "author_sort", DataType::Text, false, { "author_sort" } },
    { "comments", DataType::Text, false, { "comments", "comment" } },
    { "cover", DataType::Int, false, { "cover" } },
    { "id", DataType::Int, false, { "id" } },
    { "pages", DataType::Int, false, { "pages" } },
    { "last_modified", DataType::DateTime, false, { "last_modified" } },
    { "ondevice", DataType::Text, false, { "ondevice" } },
    { "pubdate", DataType::DateTime, false, { "pubdate" } },
    { "marked", DataType::Text, false, { "marked" } },
    { "in_tag_browser", DataType::Text, false, { "in_tag_browser" } },
    { "series_index", DataType::Float, false, { "series_index" } },
    { "series_sort", DataType::Text, false, { "series_sort" } },
    { "sort", DataType::Text, false, { "title_sort" } },
    { "size", DataType::Float, false, { "size" } },
    { "timestamp", DataType::DateTime, false, { "date" } },
    { "title", DataType::Text, false, { "title" } },
    { "uuid", DataType::Text, false, { "uuid" } },
};

const Field *fieldOf(const QString &key)
{
    for (const Field &field : fields) {
        if (key == QLatin1String(field.key))
            return &field;
    }
    return nullptr;
}

// Calibre's virtual fields: never in a book of calibredb's.
bool virtualField(const QString &key)
{
    return key == QLatin1String("marked") || key == QLatin1String("in_tag_browser");
}

enum class MatchKind { Contains, Equals, Regexp, Accents };

// _matchkind(): the kind of match a query's first character asks for, and
// the query without it, in lower case but for a regular expression.
std::pair<MatchKind, QString> matchKind(QString query)
{
    MatchKind kind = MatchKind::Contains;
    if (query.size() > 1) {
        if (query.startsWith(u'\\')) {
            query = query.mid(1);
        } else if (query.startsWith(u'=')) {
            kind = MatchKind::Equals;
            query = query.mid(1);
        } else if (query.startsWith(u'~')) {
            kind = MatchKind::Regexp;
            query = query.mid(1);
        } else if (query.startsWith(u'^')) {
            kind = MatchKind::Accents;
            query = query.mid(1);
        }
    }
    if (kind != MatchKind::Regexp)
        query = lower(query);
    return { kind, query };
}

// A query ready for _match(): the query, and what each kind of match needs
// of it.
struct TextQuery
{
    MatchKind kind = MatchKind::Contains;
    QString query;
    // For "..x": x, which an equality also finds between dots.
    QString inner;
    bool innerMatch = false;
    Folded folded;
    QRegularExpression regexp;

    TextQuery(MatchKind matchKind, QString text) : kind(matchKind), query(std::move(text))
    {
        if (query.startsWith(QLatin1String(".."))) {
            query = query.mid(1);
            inner = query.mid(1);
            innerMatch = true;
        }
        if (kind == MatchKind::Contains)
            folded = fold(query);
        else if (kind == MatchKind::Accents)
            folded = fold(query, true);
        else if (kind == MatchKind::Regexp)
            regexp = QRegularExpression(query, QRegularExpression::CaseInsensitiveOption
                                                   | QRegularExpression::UseUnicodePropertiesOption);
    }

    // The error of an invalid regular expression, as Calibre words it.
    QString error() const
    {
        if (kind != MatchKind::Regexp || regexp.isValid())
            return {};
        return tr("Invalid regular expression: '%1' with error: %2").arg(query, regexp.errorString());
    }

    // _match() of one value; `folded` is its fold, when already made.
    bool matches(const QString &value, const Folded *folded = nullptr) const
    {
        switch (kind) {
        case MatchKind::Contains:
            return contains(folded ? *folded : fold(value), this->folded);
        case MatchKind::Accents:
            return contains(fold(value, true), this->folded);
        case MatchKind::Regexp:
            return regexp.match(lower(value)).hasMatch();
        case MatchKind::Equals: {
            const QString text = lower(value);
            if (innerMatch) {
                if (query == text)
                    return true;
                for (const QString &part : text.split(u'.')) {
                    if (!blank(part) && part.trimmed() == inner)
                        return true;
                }
                return false;
            }
            if (query.startsWith(u'.')) {
                const qsizetype length = query.size() - 1;
                return text.startsWith(QStringView(query).mid(1))
                       && (text.size() == length || text.at(length) == u'.');
            }
            return query == text;
        }
        }
        return false;
    }
};

// The relational operators of DateSearch and NumericSearch, longest first.
enum class Relation { NotEqual, GreaterOrEqual, LessOrEqual, Equal, Greater, Less };

Relation relation(QString *query)
{
    static const std::pair<const char *, Relation> operators[] = {
        { "!=", Relation::NotEqual }, { ">=", Relation::GreaterOrEqual }, { "<=", Relation::LessOrEqual },
        { "=", Relation::Equal }, { ">", Relation::Greater }, { "<", Relation::Less },
    };
    for (const auto &[text, kind] : operators) {
        if (query->startsWith(QLatin1String(text))) {
            *query = query->mid(qsizetype(strlen(text)));
            return kind;
        }
    }
    return Relation::Equal;
}

template<typename T>
bool compare(Relation kind, const T &value, const T &query)
{
    switch (kind) {
    case Relation::NotEqual:
        return value != query;
    case Relation::GreaterOrEqual:
        return value >= query;
    case Relation::LessOrEqual:
        return value <= query;
    case Relation::Equal:
        return value == query;
    case Relation::Greater:
        return value > query;
    case Relation::Less:
        return value < query;
    }
    return false;
}

// DateSearch's comparisons, to the year, the month or the day.
bool dateEqual(QDate value, QDate query, int fieldCount)
{
    if (value.year() != query.year())
        return false;
    if (fieldCount == 1)
        return true;
    if (value.month() != query.month())
        return false;
    return fieldCount == 2 || value.day() == query.day();
}

bool dateGreater(QDate value, QDate query, int fieldCount)
{
    if (value.year() > query.year())
        return true;
    if (fieldCount > 1 && value.year() == query.year()) {
        if (value.month() > query.month())
            return true;
        return fieldCount == 3 && value.month() == query.month() && value.day() > query.day();
    }
    return false;
}

bool dateLess(QDate value, QDate query, int fieldCount)
{
    if (value.year() < query.year())
        return true;
    if (fieldCount > 1 && value.year() == query.year()) {
        if (value.month() < query.month())
            return true;
        return fieldCount == 3 && value.month() == query.month() && value.day() < query.day();
    }
    return false;
}

bool compareDates(Relation kind, QDate value, QDate query, int fieldCount)
{
    switch (kind) {
    case Relation::NotEqual:
        return !dateEqual(value, query, fieldCount);
    case Relation::GreaterOrEqual:
        return !dateLess(value, query, fieldCount);
    case Relation::LessOrEqual:
        return !dateGreater(value, query, fieldCount);
    case Relation::Equal:
        return dateEqual(value, query, fieldCount);
    case Relation::Greater:
        return dateGreater(value, query, fieldCount);
    case Relation::Less:
        return dateLess(value, query, fieldCount);
    }
    return false;
}

// parse_date() of a query, as far as a search uses it: its year, month and
// day, the missing ones from the current month, on the 15th.
std::optional<QDate> parseQueryDate(const QString &text)
{
    const QDate today = QDate::currentDate();
    static const QRegularExpression iso(QStringLiteral("^\\s*(\\d{4})(?:[-/](\\d{1,2})(?:[-/](\\d{1,2}))?)?\\s*$"));
    QRegularExpressionMatch match = iso.match(text);
    if (match.hasMatch()) {
        const int year = match.captured(1).toInt();
        const int month = match.captured(2).isEmpty() ? today.month() : match.captured(2).toInt();
        const int day = match.captured(3).isEmpty() ? 15 : match.captured(3).toInt();
        const QDate date(year, month, day);
        return date.isValid() ? std::optional<QDate>(date) : std::nullopt;
    }
    // Day and month in the system's order, then the year.
    static const QRegularExpression numeric(QStringLiteral("^\\s*(\\d{1,2})[-/](\\d{1,2})(?:[-/](\\d{2,4}))?\\s*$"));
    match = numeric.match(text);
    if (match.hasMatch()) {
        const QString format = QLocale::system().dateFormat(QLocale::ShortFormat);
        const bool dayFirst = format.indexOf(u'd') >= 0 && format.indexOf(u'd') < format.indexOf(u'M');
        int first = match.captured(1).toInt();
        int second = match.captured(2).toInt();
        int year = match.captured(3).isEmpty() ? today.year() : match.captured(3).toInt();
        if (match.captured(3).size() == 2)
            year += 2000;
        // "03/2015" is a month and a year.
        if (match.captured(3).isEmpty() && second > 31) {
            year = second;
            second = first;
            first = 15;
            return QDate(year, second, first).isValid() ? std::optional<QDate>(QDate(year, second, first))
                                                        : std::nullopt;
        }
        const QDate date = dayFirst ? QDate(year, second, first) : QDate(year, first, second);
        return date.isValid() ? std::optional<QDate>(date) : std::nullopt;
    }
    return std::nullopt;
}

// A word a search takes in Calibre's interface language, lower case: today,
// yesterday, thismonth, daysago, No, no, Yes, yes, unchecked, checked, empty
// and blank, by their place.
QStringList localWords(const QString &language, int which)
{
    QStringList words;
    for (const SearchWords &entry : searchWords) {
        if (language == QLatin1String(entry.language)
            || (language.size() > 2 && language.left(2) == QLatin1String(entry.language))) {
            words.append(lower(QString::fromUtf8(entry.words[which])));
            break;
        }
    }
    return words;
}

QBitArray empty(qsizetype size)
{
    return QBitArray(size);
}

} // namespace

Folded fold(QStringView text, bool keepPunctuation)
{
    const FoldTable &table = foldTable();
    const int which = keepPunctuation ? 1 : 0;
    Folded result;
    result.text.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t code = text[i].unicode();
        if (QChar::isHighSurrogate(code) && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            code = QChar::surrogateToUcs4(text[i], text[i + 1]);
            ++i;
        }
        if (code < 0x10000) {
            if (const quint16 entry = table.index[code]) {
                const FoldTable::Entry &found = table.entries[entry - 1];
                for (const int inside : found.inside[which])
                    result.inside.append(int(result.text.size()) + inside);
                result.text += found.folds[which];
                continue;
            }
        }
        appendUtf8(result.text, QChar::toLower(code));
    }
    return result;
}

bool contains(const Folded &text, const Folded &query)
{
    if (query.text.isEmpty())
        return true;
    for (qsizetype at = text.text.indexOf(query.text); at >= 0; at = text.text.indexOf(query.text, at + 1)) {
        if (!text.inside.contains(int(at)) && !text.inside.contains(int(at + query.text.size())))
            return true;
    }
    return false;
}

Preferences Preferences::fromRows(const QHash<QString, QString> &rows)
{
    Preferences preferences;
    const auto object = [&rows](const char *key) {
        return QJsonDocument::fromJson(rows.value(QLatin1String(key)).toUtf8()).object();
    };
    const QJsonObject saved = object("saved_searches");
    for (auto it = saved.begin(); it != saved.end(); ++it)
        preferences.savedSearches.insert(it.key(), it.value().toString());
    const QJsonObject libraries = object("virtual_libraries");
    for (auto it = libraries.begin(); it != libraries.end(); ++it)
        preferences.virtualLibraries.insert(it.key(), it.value().toString());
    const QJsonObject groups = object("grouped_search_terms");
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QStringList terms;
        for (const QJsonValue &term : it.value().toArray())
            terms.append(term.toString());
        preferences.groupedSearchTerms.insert(it.key(), terms);
    }
    const QJsonObject categories = object("user_categories");
    for (auto it = categories.begin(); it != categories.end(); ++it) {
        QList<QPair<QString, QString>> items;
        for (const QJsonValue &item : it.value().toArray()) {
            const QJsonArray parts = item.toArray();
            items.append({ parts.at(0).toString(), parts.at(1).toString() });
        }
        preferences.userCategories.insert(it.key(), items);
    }
    preferences.virtualLibraryOnStartup =
        QJsonDocument::fromJson("[" + rows.value(QStringLiteral("virtual_lib_on_startup")).toUtf8() + "]")
            .array().at(0).toString();
    if (rows.contains(QStringLiteral("bools_are_tristate")))
        preferences.boolsAreTristate =
            QJsonDocument::fromJson("[" + rows.value(QStringLiteral("bools_are_tristate")).toUtf8() + "]")
                .array().at(0).toBool(true);
    return preferences;
}

Index::Index(const LibraryData &data, const Preferences &preferences, const CalibreText::Tweaks &tweaks,
             const QString &interfaceLanguage)
    : m_data(data)
    , m_preferences(preferences)
    , m_tweaks(tweaks)
    , m_interfaceLanguage(interfaceLanguage)
{
    const qsizetype count = data.books.size();
    m_titles.reserve(count);
    m_comments.reserve(count);
    m_authorSorts.reserve(count);
    m_sorts.reserve(count);
    m_uuids.reserve(count);
    for (const Book &book : data.books) {
        m_titles.append(fold(book.title));
        m_comments.append(fold(book.comments));
        m_authorSorts.append(fold(book.authorSort));
        m_sorts.append(fold(book.sort));
        m_uuids.append(fold(book.uuid));
    }
    const auto items = [](const QList<FacetItem> &list, QList<Folded> *names, QHash<int, qsizetype> *rows) {
        names->reserve(list.size());
        for (qsizetype row = 0; row < list.size(); ++row) {
            names->append(fold(list.at(row).name));
            rows->insert(list.at(row).id, row);
        }
    };
    items(data.authors, &m_authorNames, &m_authorRows);
    items(data.tags, &m_tagNames, &m_tagRows);
    items(data.series, &m_seriesNames, &m_seriesRows);
    items(data.publishers, &m_publisherNames, &m_publisherRows);
    for (const CustomColumn &column : data.customColumns) {
        m_customTerms.append(u'#' + column.label);
        if (column.datatype == QLatin1String("series"))
            m_customTerms.append(u'#' + column.label + QStringLiteral("_index"));
    }
}

// Parser of db/search.py: a search over the books of an index, among a
// universal set, the books of the search restriction.
class Search
{
public:
    Search(const Index &index, QBitArray universe)
        : m_index(index)
        , m_books(index.m_data.books)
        , m_universe(std::move(universe))
    {
        for (const Field &field : fields) {
            for (const char *term : field.terms)
                m_termKeys.insert(QLatin1String(term), QLatin1String(field.key));
        }
        for (const QString &term : index.m_customTerms)
            m_termKeys.insert(term, term);
        const Preferences &preferences = index.m_preferences;
        for (auto it = preferences.userCategories.cbegin(); it != preferences.userCategories.cend(); ++it) {
            m_termKeys.insert(u'@' + it.key(), u'@' + it.key());
            m_termKeys.insert(lower(u'@' + it.key()), u'@' + it.key());
        }
        for (auto it = preferences.groupedSearchTerms.cbegin(); it != preferences.groupedSearchTerms.cend(); ++it)
            m_termKeys.insert(it.key(), it.key());
        for (auto it = m_termKeys.cbegin(); it != m_termKeys.cend(); ++it)
            m_locations.insert(lower(it.key()));
        for (const char *item : { "all", "search", "vl", "template" })
            m_locations.insert(QLatin1String(item));
    }

    QString error() const { return m_error; }
    const QSet<QString> &locations() const { return m_locations; }

    // SearchQueryParser.parse(): the books matching `query` in the universe.
    QBitArray parse(const QString &query)
    {
        m_searchesSeen.clear();
        return evaluateQuery(query, m_universe);
    }

private:
    // _parse(): parses and evaluates, among `candidates`.
    QBitArray evaluateQuery(const QString &query, const QBitArray &candidates)
    {
        Parser parser(m_locations);
        QString error;
        const Tree tree = parser.parse(query, &error);
        if (!tree) {
            fail(error);
            return empty(m_books.size());
        }
        return evaluate(*tree, candidates);
    }

    void fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    QBitArray evaluate(const Node &node, const QBitArray &candidates)
    {
        if (!m_error.isEmpty())
            return empty(m_books.size());
        switch (node.kind) {
        case Node::And: {
            // The right side looks only among what the left found.
            const QBitArray left = evaluate(*node.left, candidates);
            return left & evaluate(*node.right, left);
        }
        case Node::Or: {
            const QBitArray left = evaluate(*node.left, candidates);
            return left | evaluate(*node.right, candidates & ~left);
        }
        case Node::Not:
            return candidates & ~evaluate(*node.left, candidates);
        case Node::Term:
            break;
        }
        if (lower(node.location) == QLatin1String("search")) {
            QString name = node.query;
            if (name.startsWith(u'='))
                name = name.mid(1);
            const QString key = lower(name);
            if (m_searchesSeen.contains(key)) {
                fail(tr("Recursive saved search: %1").arg(name));
                return empty(m_books.size());
            }
            m_searchesSeen.insert(key);
            std::optional<QString> saved;
            for (auto it = m_index.m_preferences.savedSearches.cbegin();
                 it != m_index.m_preferences.savedSearches.cend(); ++it) {
                if (lower(it.key()) == key) {
                    saved = it.value();
                    break;
                }
            }
            if (!saved) {
                fail(tr("Unknown saved search: %1").arg(name));
                return empty(m_books.size());
            }
            QBitArray result = evaluateQuery(*saved, candidates);
            m_searchesSeen.remove(key);
            return result;
        }
        return matches(node.location, node.query, candidates, true);
    }

    bool none(const QBitArray &set) const { return set.count(true) == 0; }

    // get_matches().
    QBitArray matches(QString location, QString query, const QBitArray &candidates, bool allowRecursion)
    {
        QBitArray found = empty(m_books.size());
        if (!m_error.isEmpty() || none(candidates) || query.isEmpty() || blank(query))
            return found;
        if (!m_locations.contains(location))
            return found;

        const Preferences &preferences = m_index.m_preferences;
        if (location == QLatin1String("vl"))
            return candidates & virtualLibrary(query);

        if (location.size() > 2 && location.startsWith(u'@') && preferences.groupedSearchTerms.contains(location.mid(1)))
            location = location.mid(1);

        const QString originalLocation = location;
        location = m_termKeys.value(lower(location.trimmed()), lower(location.trimmed()));

        // A grouped search term: each of its fields.
        if (preferences.groupedSearchTerms.contains(location)) {
            if (!allowRecursion) {
                fail(tr("Recursive query group detected: %1").arg(query));
                return found;
            }
            bool invert = false;
            if (lower(query) == QLatin1String("false")) {
                invert = true;
                query = QStringLiteral("true");
            }
            for (const QString &field : preferences.groupedSearchTerms.value(location)) {
                found |= matches(field, query, candidates, false);
                if ((candidates & ~found).count(true) == 0)
                    break;
            }
            return invert ? m_universe & ~found : found;
        }

        if (const std::optional<Custom> custom = customOf(location)) {
            const QString &type = custom->column->datatype;
            if (custom->seriesIndex)
                return numericSearch(location, lower(query), DataType::Float, candidates);
            if (type == QLatin1String("composite")) {
                fail(tr("Cousoir does not compute composite columns, which Calibre's templates make: %1").arg(location));
                return found;
            }
            if (type == QLatin1String("datetime"))
                return dateSearch(location, lower(query), candidates);
            if (type == QLatin1String("int") || type == QLatin1String("float") || type == QLatin1String("rating"))
                return numericSearch(location, lower(query), custom->type, candidates);
            if (custom->column->multiple && query.size() > 1 && query.at(0) == u'#'
                && QStringView(u"=<>!").contains(query.at(1)))
                return countSearch(location, lower(query.mid(1)), candidates);
            if (type == QLatin1String("bool"))
                return booleanSearch(location, lower(query), candidates);
            return textSearch(location, query, candidates);
        }

        if (const Field *field = fieldOf(location)) {
            if (field->type == DataType::DateTime)
                return dateSearch(location, lower(query), candidates);
            if (field->type == DataType::Rating || field->type == DataType::Int || field->type == DataType::Float)
                return numericSearch(location, lower(query), field->type, candidates);
            if (field->multiple && query.size() > 1 && query.at(0) == u'#'
                && QStringView(u"=<>!").contains(query.at(1)))
                return countSearch(location, lower(query.mid(1)), candidates);
            if (location == QLatin1String("identifiers")) {
                if (originalLocation == QLatin1String("isbn"))
                    return keyPairSearch(QStringLiteral("=isbn:") + query, candidates);
                return keyPairSearch(query, candidates);
            }
        }

        if (location.size() >= 2 && location.startsWith(u'@'))
            return userCategory(location.mid(1), lower(query), candidates);

        if (location == QLatin1String("template")) {
            fail(tr("Cousoir does not run Calibre's templates in searches"));
            return found;
        }
        return textSearch(location, query, candidates);
    }

    // books_in_virtual_library(): the virtual library's search, over all the
    // books.
    QBitArray virtualLibrary(const QString &name)
    {
        const auto library = m_index.m_preferences.virtualLibraries.constFind(name);
        if (library == m_index.m_preferences.virtualLibraries.cend() || library->isEmpty()) {
            fail(tr("No such Virtual library: %1").arg(name));
            return empty(m_books.size());
        }
        if (m_librariesSeen.contains(name)) {
            fail(tr("Virtual library search is recursive: %1").arg(name));
            return empty(m_books.size());
        }
        m_librariesSeen.insert(name);
        Search inner(m_index, QBitArray(m_books.size(), true));
        inner.m_librariesSeen = m_librariesSeen;
        QBitArray result = inner.parse(library->trimmed());
        m_librariesSeen.remove(name);
        if (!inner.error().isEmpty())
            fail(inner.error());
        return result;
    }

    // get_user_category_matches(): the books with any item of the category,
    // or of its subcategories with a query starting with a dot.
    QBitArray userCategory(const QString &category, QString query, const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        if (query.size() < 2)
            return found;
        const bool subcategories = query.startsWith(u'.');
        if (subcategories)
            query = query.mid(1);
        QBitArray remaining = candidates;
        const auto &categories = m_index.m_preferences.userCategories;
        for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
            if (it.key() != category && !(subcategories && it.key().startsWith(category + u'.')))
                continue;
            for (const auto &[item, field] : it.value()) {
                const QBitArray items = matches(field, u'=' + item, remaining, true);
                remaining &= ~items;
                found |= items;
            }
        }
        return query == QLatin1String("false") ? candidates & ~found : found;
    }

    // The books with a value of an item field matching `test`: authors,
    // tags, series, publishers, by their names; languages by their codes.
    template<typename Test>
    QBitArray itemSearch(const QString &location, const QBitArray &candidates, Test test)
    {
        const LibraryData &data = m_index.m_data;
        QBitArray found = empty(m_books.size());
        const auto byItems = [&](const QList<FacetItem> &items, const QList<Folded> *names,
                                 const auto &bookItems) {
            QSet<int> matching;
            for (qsizetype row = 0; row < items.size(); ++row) {
                const FacetItem &item = items.at(row);
                const QString &value = location == QLatin1String("languages") ? item.sort : item.name;
                if (test(value, names ? &names->at(row) : nullptr))
                    matching.insert(item.id);
            }
            if (matching.isEmpty())
                return;
            for (qsizetype row = 0; row < m_books.size(); ++row) {
                if (!candidates.testBit(row))
                    continue;
                for (const int id : bookItems(m_books.at(row))) {
                    if (matching.contains(id)) {
                        found.setBit(row);
                        break;
                    }
                }
            }
        };
        const auto single = [](int id) { return id > 0 ? QList<int>{ id } : QList<int>(); };
        if (location == QLatin1String("authors"))
            byItems(data.authors, &m_index.m_authorNames, [](const Book &book) { return book.authorIds; });
        else if (location == QLatin1String("tags"))
            byItems(data.tags, &m_index.m_tagNames, [](const Book &book) { return book.tagIds; });
        else if (location == QLatin1String("series"))
            byItems(data.series, &m_index.m_seriesNames, [&](const Book &book) { return single(book.seriesId); });
        else if (location == QLatin1String("publisher"))
            byItems(data.publishers, &m_index.m_publisherNames,
                    [&](const Book &book) { return single(book.publisherId); });
        else if (location == QLatin1String("languages"))
            byItems(data.languages, nullptr, [](const Book &book) { return book.languageIds; });
        return found;
    }

    // The values of a book field, and their folds when made once, for the
    // text fields that are not items.
    template<typename Test>
    QBitArray bookSearch(const QString &location, const QBitArray &candidates, Test test)
    {
        QBitArray found = empty(m_books.size());
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (!candidates.testBit(row))
                continue;
            const Book &book = m_books.at(row);
            bool hit = false;
            if (location == QLatin1String("title")) {
                hit = test(book.title, &m_index.m_titles.at(row));
            } else if (location == QLatin1String("comments")) {
                hit = !book.comments.isEmpty() && test(book.comments, &m_index.m_comments.at(row));
            } else if (location == QLatin1String("author_sort")) {
                hit = test(book.authorSort, &m_index.m_authorSorts.at(row));
            } else if (location == QLatin1String("sort")) {
                hit = test(book.sort, &m_index.m_sorts.at(row));
            } else if (location == QLatin1String("uuid")) {
                hit = test(book.uuid, &m_index.m_uuids.at(row));
            } else if (location == QLatin1String("formats")) {
                for (const BookFormat &format : book.formats)
                    hit = hit || test(format.format, nullptr);
            } else if (location == QLatin1String("identifiers")) {
                // Searched as text, a book's identifiers are their types.
                for (const auto &identifier : book.identifiers)
                    hit = hit || test(identifier.first, nullptr);
            } else if (const std::optional<Custom> custom = customOf(location)) {
                const QVariant value = customValue(book, *custom);
                if (custom->column->multiple) {
                    for (const QString &item : value.toStringList())
                        hit = hit || test(item, nullptr);
                } else if (!value.isNull()) {
                    hit = test(value.toString(), nullptr);
                }
            }
            if (hit)
                found.setBit(row);
        }
        return found;
    }

    // A custom column a search term names, "#genre", or its series' numbers,
    // "#saga_index"; and the kind of values a search compares.
    struct Custom
    {
        qsizetype index = -1;
        const CustomColumn *column = nullptr;
        bool seriesIndex = false;
        DataType type = DataType::None;
    };

    std::optional<Custom> customOf(const QString &key) const
    {
        if (!key.startsWith(u'#'))
            return std::nullopt;
        const QList<CustomColumn> &columns = m_index.m_data.customColumns;
        for (qsizetype index = 0; index < columns.size(); ++index) {
            const CustomColumn &column = columns.at(index);
            const bool numbers = column.datatype == QLatin1String("series")
                && key == u'#' + column.label + QStringLiteral("_index");
            if (key != u'#' + column.label && !numbers)
                continue;
            Custom custom{ index, &column, numbers, DataType::Text };
            const QString &type = column.datatype;
            if (numbers || type == QLatin1String("float"))
                custom.type = DataType::Float;
            else if (type == QLatin1String("int"))
                custom.type = DataType::Int;
            else if (type == QLatin1String("rating"))
                custom.type = DataType::Rating;
            else if (type == QLatin1String("datetime"))
                custom.type = DataType::DateTime;
            else if (type == QLatin1String("bool") || type == QLatin1String("composite"))
                custom.type = DataType::None;
            return custom;
        }
        return std::nullopt;
    }

    // A book's value of a custom column: null when it has none.
    QVariant customValue(const Book &book, const Custom &custom) const
    {
        const CustomValue value = book.custom.value(custom.index);
        if (custom.seriesIndex)
            return value.value.isNull() ? QVariant() : QVariant(value.seriesIndex);
        return value.value;
    }

    // Whether a custom column's rating counts half stars, which a search
    // compares as they are rather than in whole stars.
    static bool halfStars(const CustomColumn &column)
    {
        return column.datatype == QLatin1String("rating")
            && QJsonDocument::fromJson(column.display).object().value(QLatin1String("allow_half_stars")).toBool();
    }

    // BooleanSearch: yes, no, or none when bools_are_tristate.
    QBitArray booleanSearch(const QString &field, const QString &query, const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        const QString language = m_index.m_interfaceLanguage;
        // local_forms(): the translation in lower case, composed and not.
        const auto local = [&language](std::initializer_list<int> which) {
            QSet<QString> forms;
            for (const int index : which) {
                for (const QString &word : localWords(language, index)) {
                    for (const auto form : { QString::NormalizationForm_C, QString::NormalizationForm_D,
                                             QString::NormalizationForm_KC, QString::NormalizationForm_KD })
                        forms.insert(word.normalized(form));
                }
            }
            return forms;
        };
        const QSet<QString> no = QSet<QString>{ QStringLiteral("unchecked"), QStringLiteral("_unchecked"),
                                                QStringLiteral("no"), QStringLiteral("_no") } + local({ 4, 5, 8 });
        const QSet<QString> noAndFalse = no + QSet<QString>{ QStringLiteral("false") };
        const QSet<QString> noAndTrue = no + QSet<QString>{ QStringLiteral("true") };
        const QSet<QString> yes = QSet<QString>{ QStringLiteral("checked"), QStringLiteral("_checked"),
                                                 QStringLiteral("yes"), QStringLiteral("_yes"), QStringLiteral("true") }
            + local({ 6, 7, 9 });
        const QSet<QString> blank = QSet<QString>{ QStringLiteral("blank"), QStringLiteral("_blank"),
                                                   QStringLiteral("empty"), QStringLiteral("_empty"),
                                                   QStringLiteral("false") } + local({ 10, 11 });
        if (!(noAndFalse + yes + blank).contains(query)) {
            fail(tr("Invalid boolean query \"%1\"").arg(query));
            return found;
        }
        const Custom custom = *customOf(field);
        const bool tristate = m_index.m_preferences.boolsAreTristate;
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (!candidates.testBit(row))
                continue;
            const QVariant value = customValue(m_books.at(row), custom);
            bool hit = false;
            if (!tristate)
                hit = value.isNull() || !value.toBool() ? noAndFalse.contains(query) : yes.contains(query);
            else if (value.isNull())
                hit = blank.contains(query);
            else if (!value.toBool())
                hit = noAndTrue.contains(query);
            else
                hit = yes.contains(query);
            if (hit)
                found.setBit(row);
        }
        return found;
    }

    static bool itemField(const QString &location)
    {
        return location == QLatin1String("authors") || location == QLatin1String("tags")
            || location == QLatin1String("series") || location == QLatin1String("publisher")
            || location == QLatin1String("languages");
    }

    // The text branch of get_matches(): `all` or one text field.
    QBitArray textSearch(const QString &location, const QString &rawQuery, const QBitArray &candidates)
    {
        const auto [kind, query] = matchKind(rawQuery);
        QStringList locations;
        if (location == QLatin1String("all")) {
            for (const Field &field : fields) {
                const QString key = QLatin1String(field.key);
                if (key != QLatin1String("series_sort") && key != QLatin1String("id") && key != QLatin1String("uuid")
                    && !virtualField(key))
                    locations.append(key);
            }
            // The custom columns but a composite, which Cousoir cannot
            // compute.
            for (const QString &term : m_index.m_customTerms) {
                if (customOf(term)->column->datatype != QLatin1String("composite"))
                    locations.append(term);
            }
        } else {
            locations.append(location);
        }

        bool numberOk = false;
        const double number = query.toDouble(&numberOk);
        const std::optional<qint64> intQuery = numberOk && std::isfinite(number)
            ? std::optional<qint64>(qint64(number)) : std::nullopt;

        QBitArray found = empty(m_books.size());
        for (const QString &field : std::as_const(locations)) {
            const QBitArray current = candidates & ~found;
            QString q = query;
            if (field == QLatin1String("languages")) {
                const QString code = CalibreText::canonicalizeLanguage(query);
                if (!code.isEmpty())
                    q = code;
            }

            if (kind == MatchKind::Contains && (lower(q) == QLatin1String("true") || lower(q) == QLatin1String("false"))) {
                const QBitArray present = presence(field, current);
                found |= lower(q) == QLatin1String("true") ? present : current & ~present;
                continue;
            }

            const Field *metadata = fieldOf(field);
            const std::optional<Custom> custom = customOf(field);
            const DataType type = custom ? custom->type
                : metadata && field != QLatin1String("series_sort") ? metadata->type : DataType::None;
            if (type == DataType::Rating || type == DataType::Float || type == DataType::Int) {
                if (intQuery || numberOk)
                    found |= numberInAll(field, type, number, intQuery, current);
                continue;
            }

            if (field == QLatin1String("series_sort")) {
                found |= seriesSortSearch(TextQuery(kind, q), current);
                continue;
            }
            if (type == DataType::None || type == DataType::DateTime)
                continue;

            const TextQuery text(kind, q);
            if (const QString error = text.error(); !error.isEmpty()) {
                fail(error);
                return empty(m_books.size());
            }
            const auto test = [&text](const QString &value, const Folded *folded) { return text.matches(value, folded); };
            found |= itemField(field) ? itemSearch(field, current, test) : bookSearch(field, current, test);
        }
        return found;
    }

    // The books with a value in the field, for "true" and "false".
    QBitArray presence(const QString &field, const QBitArray &candidates)
    {
        const auto any = [](const QString &value, const Folded *) { return !blank(value); };
        if (itemField(field))
            return itemSearch(field, candidates, any);
        QBitArray found = empty(m_books.size());
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (!candidates.testBit(row))
                continue;
            const Book &book = m_books.at(row);
            bool present = false;
            if (field == QLatin1String("rating"))
                present = book.rating > 0;
            else if (field == QLatin1String("cover"))
                present = book.hasCover;
            else if (field == QLatin1String("pages"))
                present = book.pages > 0;
            else if (field == QLatin1String("series_index"))
                present = book.seriesIndex != 0;
            else if (field == QLatin1String("size"))
                present = maxSize(book).value_or(0) != 0;
            else if (field == QLatin1String("timestamp") || field == QLatin1String("pubdate")
                     || field == QLatin1String("last_modified"))
                present = !dateOf(book, field).isNull();
            else if (field == QLatin1String("identifiers"))
                present = !book.identifiers.isEmpty();
            else if (field == QLatin1String("formats"))
                present = std::any_of(book.formats.cbegin(), book.formats.cend(),
                                      [](const BookFormat &format) { return !blank(format.format); });
            else if (field == QLatin1String("title"))
                present = !blank(book.title);
            else if (field == QLatin1String("comments"))
                present = !blank(book.comments);
            else if (field == QLatin1String("author_sort"))
                present = !blank(book.authorSort);
            else if (field == QLatin1String("sort"))
                present = !blank(book.sort);
            else if (field == QLatin1String("uuid"))
                present = !blank(book.uuid);
            else if (field == QLatin1String("series_sort"))
                present = book.seriesId > 0;
            else if (const std::optional<Custom> custom = customOf(field)) {
                const QVariant value = customValue(book, *custom);
                if (custom->column->multiple) {
                    const QStringList items = value.toStringList();
                    present = std::any_of(items.cbegin(), items.cend(), [](const QString &item) { return !blank(item); });
                } else if (custom->type == DataType::Text) {
                    present = !value.isNull() && !blank(value.toString());
                } else if (custom->type == DataType::DateTime) {
                    present = !value.isNull();
                } else {
                    present = !value.isNull() && value.toDouble() != 0;
                }
            }
            if (present)
                found.setBit(row);
        }
        return found;
    }

    // A number searched in `all`: a rating in stars, a float or an integer,
    // equal.
    QBitArray numberInAll(const QString &field, DataType type, double number, std::optional<qint64> integer,
                          const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (!candidates.testBit(row))
                continue;
            const Book &book = m_books.at(row);
            bool hit = false;
            if (const std::optional<Custom> custom = customOf(field)) {
                const QVariant value = customValue(book, *custom);
                if (value.isNull())
                    hit = false;
                else if (type == DataType::Rating)
                    hit = integer && value.toLongLong() == *integer * 2;
                else if (type == DataType::Float)
                    hit = value.toDouble() == number;
                else
                    hit = integer && value.toLongLong() == *integer;
            } else if (type == DataType::Rating)
                hit = integer && book.rating > 0 && book.rating == *integer * 2;
            else if (type == DataType::Float) {
                const std::optional<double> value = field == QLatin1String("size")
                    ? (maxSize(book) ? std::optional<double>(double(*maxSize(book))) : std::nullopt)
                    : std::optional<double>(book.seriesIndex);
                hit = value && *value == number;
            } else if (integer) {
                if (field == QLatin1String("cover"))
                    hit = (book.hasCover ? 1 : 0) == *integer;
                else if (field == QLatin1String("pages"))
                    hit = book.pages >= 0 && book.pages == *integer;
            }
            if (hit)
                found.setBit(row);
        }
        return found;
    }

    // The largest file of the book: Calibre's size.
    static std::optional<qint64> maxSize(const Book &book)
    {
        if (book.formats.isEmpty())
            return std::nullopt;
        qint64 size = 0;
        for (const BookFormat &format : book.formats)
            size = std::max(size, format.size);
        return size;
    }

    QDateTime dateOf(const Book &book, const QString &field) const
    {
        if (const std::optional<Custom> custom = customOf(field))
            return customValue(book, *custom).toDateTime();
        if (field == QLatin1String("timestamp"))
            return book.timestamp;
        if (field == QLatin1String("pubdate"))
            return book.pubdate;
        return book.lastModified;
    }

    // SeriesField.iter_searchable_values_for_sort(): each series' title sort,
    // in the language most of its books among the candidates have first.
    QBitArray seriesSortSearch(const TextQuery &text, const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        QHash<int, QList<qsizetype>> booksOf;
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (candidates.testBit(row) && m_books.at(row).seriesId > 0)
                booksOf[m_books.at(row).seriesId].append(row);
        }
        for (const FacetItem &series : m_index.m_data.series) {
            const QList<qsizetype> rows = booksOf.value(series.id);
            if (rows.isEmpty())
                continue;
            QMap<QString, int> counts;
            QStringList order;
            for (const qsizetype row : rows) {
                const QStringList &languages = m_books.at(row).languages;
                if (languages.isEmpty())
                    continue;
                if (!counts.contains(languages.first()))
                    order.append(languages.first());
                ++counts[languages.first()];
            }
            QString language;
            int most = 0;
            for (const QString &candidate : std::as_const(order)) {
                if (counts.value(candidate) > most) {
                    most = counts.value(candidate);
                    language = candidate;
                }
            }
            const QString sort = CalibreText::titleSort(series.name, language, m_index.m_tweaks,
                                                        m_index.m_interfaceLanguage);
            if (text.matches(sort)) {
                for (const qsizetype row : rows)
                    found.setBit(row);
            }
        }
        return found;
    }

    // DateSearch.
    QBitArray dateSearch(const QString &field, QString query, const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        if (query.size() < 2)
            return found;
        if (query == QLatin1String("false") || query == QLatin1String("true")) {
            const bool wanted = query == QLatin1String("true");
            for (qsizetype row = 0; row < m_books.size(); ++row) {
                if (!candidates.testBit(row))
                    continue;
                const QDateTime date = dateOf(m_books.at(row), field);
                const bool defined = date.isValid() && !CalibreDb::isUndefinedDate(date)
                                     && date > CalibreDb::undefinedDate();
                if (defined == wanted)
                    found.setBit(row);
            }
            return found;
        }

        const Relation kind = relation(&query);
        const QString language = m_index.m_interfaceLanguage;
        const auto is = [&](int which, const char *english) {
            QStringList words = localWords(language, which);
            words << QLatin1String(english) << u'_' + QLatin1String(english);
            return words;
        };
        QDate date;
        int fieldCount = 3;
        const QDate today = QDate::currentDate();
        static const QRegularExpression number(QStringLiteral("^-?\\d+$"));
        QString daysAgo;
        for (const QString &word : is(3, "daysago")) {
            if (!word.isEmpty() && query.endsWith(word)) {
                daysAgo = word;
                break;
            }
        }
        if (is(0, "today").contains(query)) {
            date = today;
        } else if (is(1, "yesterday").contains(query)) {
            date = today.addDays(-1);
        } else if (is(2, "thismonth").contains(query)) {
            date = today;
            fieldCount = 2;
        } else if (!daysAgo.isEmpty()) {
            const QString count = query.chopped(daysAgo.size());
            if (!number.match(count).hasMatch()) {
                fail(tr("Number conversion error: %1").arg(count));
                return found;
            }
            date = today.addDays(-count.toInt());
        } else {
            const std::optional<QDate> parsed = parseQueryDate(query);
            if (!parsed) {
                fail(tr("Date conversion error: %1").arg(query));
                return found;
            }
            date = *parsed;
            fieldCount = int(query.contains(u'-') ? query.count(u'-') + 1 : query.count(u'/') + 1);
        }
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (!candidates.testBit(row))
                continue;
            const QDateTime value = dateOf(m_books.at(row), field);
            if (value.isValid() && compareDates(kind, value.toLocalTime().date(), date, fieldCount))
                found.setBit(row);
        }
        return found;
    }

    // NumericSearch, for ratings, numbers and the book's id.
    QBitArray numericSearch(const QString &field, QString query, DataType type, const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        if (query.isEmpty())
            return found;
        // A rating is an item of the book, none for most; a custom column's
        // with half stars compares as it is.
        const std::optional<Custom> custom = customOf(field);
        const bool half = custom && halfStars(*custom->column);
        const bool many = field == QLatin1String("rating") || (custom && custom->type == DataType::Rating);
        const auto value = [&](const Book &book) -> std::optional<double> {
            if (custom) {
                const QVariant value = customValue(book, *custom);
                return value.isNull() ? std::nullopt : std::optional<double>(value.toDouble());
            }
            if (field == QLatin1String("rating"))
                return book.rating > 0 ? std::optional<double>(book.rating) : std::nullopt;
            if (field == QLatin1String("cover"))
                return book.hasCover ? 1 : 0;
            if (field == QLatin1String("id"))
                return book.id;
            if (field == QLatin1String("pages"))
                return book.pages >= 0 ? std::optional<double>(book.pages) : std::nullopt;
            if (field == QLatin1String("series_index"))
                return book.seriesIndex;
            if (field == QLatin1String("size")) {
                const std::optional<qint64> size = maxSize(book);
                return size ? std::optional<double>(double(*size)) : std::nullopt;
            }
            return std::nullopt;
        };

        if (many && (query == QLatin1String("true") || query == QLatin1String("false"))) {
            QBitArray rated = empty(m_books.size());
            for (qsizetype row = 0; row < m_books.size(); ++row) {
                const std::optional<double> v = candidates.testBit(row) ? value(m_books.at(row)) : std::nullopt;
                if (v && (half || *v > 0))
                    rated.setBit(row);
            }
            return query == QLatin1String("true") ? rated : candidates & ~rated;
        }

        enum class Test { Compare, Missing, Present, Falsy, Truthy };
        Test test = Test::Compare;
        Relation kind = Relation::Equal;
        double target = 0;
        const bool cover = field == QLatin1String("cover");
        if (query == QLatin1String("false")) {
            test = cover ? Test::Falsy : Test::Missing;
        } else if (query == QLatin1String("true")) {
            test = cover ? Test::Truthy : Test::Present;
        } else {
            kind = relation(&query);
            double multiplier = 1.0;
            if (query.size() > 1) {
                const QChar last = query.back().toLower();
                multiplier = last == u'k' ? 1024.0 : last == u'm' ? 1024.0 * 1024 : last == u'g' ? 1024.0 * 1024 * 1024 : 1.0;
                if (multiplier != 1.0)
                    query.chop(1);
            }
            bool ok = false;
            if (type == DataType::Float || half) {
                target = query.trimmed().toDouble(&ok);
            } else {
                // int(), which takes no decimals.
                static const QRegularExpression integer(QStringLiteral("^\\s*[+-]?\\d+\\s*$"));
                ok = integer.match(query).hasMatch();
                target = query.trimmed().toDouble();
            }
            if (!ok) {
                fail(tr("Non-numeric value in query: %1").arg(query));
                return found;
            }
            target *= multiplier;
            if (half)
                target = std::nearbyint(target * 2);
        }

        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (!candidates.testBit(row))
                continue;
            const std::optional<double> v = value(m_books.at(row));
            bool hit = false;
            switch (test) {
            case Test::Missing:
                hit = !v;
                break;
            case Test::Present:
                hit = v.has_value();
                break;
            case Test::Falsy:
                hit = !v || *v == 0;
                break;
            case Test::Truthy:
                hit = v && *v != 0;
                break;
            case Test::Compare:
                if (v) {
                    double number = *v;
                    // Ratings compare in whole stars.
                    if (type == DataType::Rating && !half && number != 0)
                        number = std::floor(number / 2);
                    hit = compare(kind, number, target);
                }
                break;
            }
            if (hit)
                found.setBit(row);
        }
        return found;
    }

    // The number of values of a book, compared: tags:#>2.
    QBitArray countSearch(const QString &field, QString query, const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        if (query.isEmpty())
            return found;
        const auto count = [&](const Book &book) -> qsizetype {
            if (field == QLatin1String("authors"))
                return book.authorIds.size();
            if (field == QLatin1String("languages"))
                return book.languageIds.size();
            if (field == QLatin1String("formats"))
                return book.formats.size();
            if (field == QLatin1String("tags"))
                return book.tagIds.size();
            if (field == QLatin1String("identifiers"))
                return book.identifiers.size();
            if (const std::optional<Custom> custom = customOf(field))
                return customValue(book, *custom).toStringList().size();
            return 0;
        };
        if (query == QLatin1String("true") || query == QLatin1String("false")) {
            for (qsizetype row = 0; row < m_books.size(); ++row) {
                // A count is never missing.
                if (candidates.testBit(row) && query == QLatin1String("true"))
                    found.setBit(row);
            }
            return found;
        }
        const Relation kind = relation(&query);
        double multiplier = 1.0;
        if (query.size() > 1) {
            const QChar last = query.back().toLower();
            multiplier = last == u'k' ? 1024.0 : last == u'm' ? 1024.0 * 1024 : last == u'g' ? 1024.0 * 1024 * 1024 : 1.0;
            if (multiplier != 1.0)
                query.chop(1);
        }
        static const QRegularExpression integer(QStringLiteral("^\\s*[+-]?\\d+\\s*$"));
        if (!integer.match(query).hasMatch()) {
            fail(tr("Non-numeric value in query: %1").arg(query));
            return found;
        }
        const double target = query.trimmed().toDouble() * multiplier;
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (candidates.testBit(row) && compare(kind, double(count(m_books.at(row))), target))
                found.setBit(row);
        }
        return found;
    }

    // KeyPairSearch: identifiers as type:value.
    QBitArray keyPairSearch(const QString &query, const QBitArray &candidates)
    {
        QBitArray found = empty(m_books.size());
        std::optional<TextQuery> key;
        QString valueText;
        MatchKind valueKind;
        if (query.contains(u':')) {
            const qsizetype colon = query.indexOf(u':');
            const auto [keyKind, keyText] = matchKind(query.left(colon).trimmed());
            std::tie(valueKind, valueText) = matchKind(query.mid(colon + 1).trimmed());
            key.emplace(keyKind, keyText);
        } else {
            std::tie(valueKind, valueText) = matchKind(query);
        }

        if (valueText == QLatin1String("true") || valueText == QLatin1String("false")) {
            QBitArray present = empty(m_books.size());
            for (qsizetype row = 0; row < m_books.size(); ++row) {
                if (!candidates.testBit(row))
                    continue;
                const Book &book = m_books.at(row);
                bool hit = false;
                if (key && !key->query.isEmpty()) {
                    for (const auto &identifier : book.identifiers)
                        hit = hit || (identifier.first == key->query && !identifier.second.isEmpty());
                } else {
                    hit = !book.identifiers.isEmpty();
                }
                if (hit)
                    present.setBit(row);
            }
            return valueText == QLatin1String("true") ? present : candidates & ~present;
        }

        const TextQuery value(valueKind, valueText);
        for (const TextQuery *text : std::initializer_list<const TextQuery *>{ key ? &*key : nullptr, &value }) {
            if (text && !text->error().isEmpty()) {
                fail(text->error());
                return found;
            }
        }
        for (qsizetype row = 0; row < m_books.size(); ++row) {
            if (!candidates.testBit(row))
                continue;
            for (const auto &[type, text] : m_books.at(row).identifiers) {
                if (key && !key->query.isEmpty() && !key->matches(type))
                    continue;
                if (!value.query.isEmpty() && !value.matches(text))
                    continue;
                found.setBit(row);
                break;
            }
        }
        return found;
    }

    const Index &m_index;
    const QList<Book> &m_books;
    QBitArray m_universe;
    QHash<QString, QString> m_termKeys;
    QSet<QString> m_locations;
    QSet<QString> m_searchesSeen;
    QSet<QString> m_librariesSeen;
    QString m_error;
};

Result search(const Index &index, const QString &query, const QString &restriction)
{
    Result result;
    const QBitArray all(index.size(), true);
    QBitArray restricted = all;
    if (!restriction.trimmed().isEmpty()) {
        Search search(index, all);
        restricted = search.parse(restriction.trimmed());
        if (!search.error().isEmpty()) {
            result.error = search.error();
            result.rows = QBitArray(index.size());
            return result;
        }
    }
    return search(index, query, restricted);
}

Result search(const Index &index, const QString &query, const QBitArray &universe)
{
    Result result;
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        result.rows = universe;
        return result;
    }
    Search search(index, universe);
    result.rows = search.parse(trimmed);
    result.error = search.error();
    if (!result.ok())
        result.rows = QBitArray(index.size());
    return result;
}

QStringList locations(const Index &index)
{
    Search search(index, QBitArray(index.size()));
    QStringList result = search.locations().values();
    result.sort();
    return result;
}

} // namespace CalibreSearch
