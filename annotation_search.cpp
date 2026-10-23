#include "annotation_search.h"

// Ported from Calibre 9.16, Copyright (C) Kovid Goyal, GPL v3:
// - src/calibre/db/backend.py: all_annotations(), search_annotations();
// - src/calibre/gui2/library/annotations.py: sorted_items(), ChapterGroup,
//   render_highlight_as_text(), render_bookmark_as_text(),
//   render_timestamp(), ExportDialog.exported_data() for text and
//   Markdown;
// - src/calibre/ebooks/epub/cfi/parse.py: cfi_sort_key(), its path and
//   offsets.

#include <QCollator>
#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

#include "book.h"
#include "calibre_languages.h"
#include "sqlite_db.h"

using PythonJson::Value;

namespace AnnotationSearch {

namespace {

constexpr int browseLimit = 4096;

// The annotation's JSON, null when it is none or was removed.
std::optional<Value> parsedAnnotation(const QByteArray &json)
{
    bool ok = false;
    Value annotation = PythonJson::parse(json, &ok);
    if (!ok || annotation.kind != Value::Kind::Dict)
        return std::nullopt;
    const Value *removed = annotation.find(QStringLiteral("removed"));
    if (removed && PythonJson::truthy(*removed))
        return std::nullopt;
    return annotation;
}

// cfi_sort_key(): the steps of the path, then the character offset, as
// numbers; an assertion in brackets aside.
QList<qint64> cfiKey(QString cfi)
{
    if (cfi.startsWith(QLatin1String("epubcfi(")))
        cfi = cfi.mid(8).chopped(cfi.endsWith(u')') ? 1 : 0);
    static const QRegularExpression assertion(QStringLiteral("\\[[^\\]]*\\]"));
    cfi.remove(assertion);
    QList<qint64> key;
    static const QRegularExpression number(QStringLiteral("([/:~@])(\\d+)"));
    for (auto it = number.globalMatch(cfi); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (match.captured(1) == QLatin1String("/") || match.captured(1) == QLatin1String(":"))
            key.append(match.captured(2).toLongLong());
    }
    return key;
}

QList<qint64> sortKey(const Hit &hit)
{
    // sorted_items(): a highlight by its spine item and CFI, a bookmark by
    // its position, anything else last.
    constexpr qint64 lastSpine = 999999999;
    const Value &annotation = hit.annotation;
    const QString type = annotation.text(QStringLiteral("type"));
    if (type == QLatin1String("highlight")) {
        const QString cfi = annotation.text(QStringLiteral("start_cfi"));
        if (!cfi.isEmpty()) {
            const Value *spine = annotation.find(QStringLiteral("spine_index"));
            const qint64 index = spine ? spine->integer : lastSpine;
            return cfiKey(QStringLiteral("/%1%2").arg(2 * (index + 1)).arg(cfi));
        }
    } else if (type == QLatin1String("bookmark") && annotation.text(QStringLiteral("pos_type")) == QLatin1String("epubcfi")) {
        return cfiKey(annotation.text(QStringLiteral("pos")));
    }
    return { lastSpine };
}

QString renderTimestamp(const QString &timestamp)
{
    const QDateTime date = QDateTime::fromString(timestamp, Qt::ISODateWithMs).toLocalTime();
    const QLocale locale = QLocale::system();
    return locale.toString(date, locale.dateTimeFormat(QLocale::ShortFormat));
}

// ChapterGroup: a book's annotations under the chapters they are in.
struct ChapterGroup
{
    QString title;
    int level = 0;
    QList<const Hit *> annotations;
    QList<std::shared_ptr<ChapterGroup>> subgroups;

    ChapterGroup *groupFor(const QString &name)
    {
        for (const auto &group : std::as_const(subgroups)) {
            if (group->title == name)
                return group.get();
        }
        auto group = std::make_shared<ChapterGroup>();
        group->title = name;
        group->level = level + 1;
        subgroups.append(group);
        return group.get();
    }

    void add(const Hit *hit, const QString &unknownChapter)
    {
        ChapterGroup *node = this;
        const Value *titles = hit->annotation.find(QStringLiteral("toc_family_titles"));
        if (!titles) {
            node = node->groupFor(unknownChapter);
        } else {
            for (const Value &chapter : titles->items)
                node = node->groupFor(chapter.string);
        }
        node->annotations.append(hit);
    }

    void render(QStringList *lines, bool markdown) const
    {
        if (!title.isEmpty()) {
            lines->append(QString(u'#').repeated(level) + u' ' + title);
            lines->append(QString());
        }
        for (const Hit *hit : annotations) {
            const Value &annotation = hit->annotation;
            QString date = renderTimestamp(annotation.text(QStringLiteral("timestamp")));
            if (annotation.text(QStringLiteral("type")) == QLatin1String("bookmark")) {
                lines->append(annotation.text(QStringLiteral("title")));
                lines->append(date);
            } else {
                lines->append(annotation.text(QStringLiteral("highlighted_text")));
                lines->append(markdown ? u'*' + date + u'*' : date);
                const QString notes = annotation.text(QStringLiteral("notes"));
                if (!notes.isEmpty()) {
                    lines->append(QString());
                    lines->append(notes);
                }
            }
            lines->append(QString());
            lines->append(markdown ? QString(u'-').repeated(20) : QStringLiteral("───"));
            lines->append(QString());
        }
        for (const auto &group : subgroups)
            group->render(lines, markdown);
    }
};

} // namespace

QList<Hit> find(SqliteDb &db, const Query &query, QString *error)
{
    QList<Hit> hits;
    const QString text = query.text.trimmed().normalized(QString::NormalizationForm_C);
    QString sql;
    QVariantList values;
    if (text.isEmpty()) {
        // all_annotations(): the newest first.
        sql = QStringLiteral("SELECT id, book, format, user_type, user, annot_data FROM annotations");
        if (!query.type.isEmpty()) {
            sql += QStringLiteral(" WHERE  annot_type = ? ");
            values.append(query.type);
        }
        sql += QStringLiteral(" ORDER BY timestamp DESC ");
    } else {
        // search_annotations(): the best first, a snippet around the words
        // found.
        const QString table = query.stemming ? QStringLiteral("annotations_fts_stemmed") : QStringLiteral("annotations_fts");
        sql = QStringLiteral("SELECT annotations.id, annotations.book, annotations.format, annotations.user_type, "
                             "annotations.user, annotations.annot_data, snippet(%1, 0, ?, ?, '…', 64) FROM annotations "
                             "JOIN %1 ON annotations.id = %1.rowid WHERE %1 MATCH ?")
                  .arg(table);
        values = { QString(markStart), QString(markEnd), text };
        if (!query.type.isEmpty()) {
            sql += QStringLiteral(" AND annotations.annot_type = ? ");
            values.append(query.type);
        }
        sql += QStringLiteral(" ORDER BY %1.rank ").arg(table);
    }
    SqliteStatement statement(db, sql);
    if (!statement.isValid()) {
        *error = statement.error();
        return {};
    }
    statement.bind(values);
    while (statement.step()) {
        const std::optional<Value> annotation = parsedAnnotation(statement.blob(5));
        if (!annotation)
            continue;
        Hit hit;
        hit.id = int(statement.integer(0));
        hit.bookId = int(statement.integer(1));
        hit.format = statement.text(2);
        hit.userType = statement.text(3);
        hit.user = statement.text(4);
        hit.annotation = *annotation;
        if (!text.isEmpty()) {
            hit.text = statement.text(6);
        } else {
            const QString type = annotation->text(QStringLiteral("type"));
            if (type == QLatin1String("bookmark"))
                hit.text = annotation->text(QStringLiteral("title"));
            else if (type == QLatin1String("highlight"))
                hit.text = annotation->text(QStringLiteral("highlighted_text"));
        }
        hits.append(hit);
        if (text.isEmpty() && hits.size() >= browseLimit)
            break;
    }
    if (!statement.error().isEmpty()) {
        *error = statement.error();
        return {};
    }
    return hits;
}

QList<Hit> sortedInBook(QList<Hit> hits)
{
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return sortKey(a) < sortKey(b); });
    return hits;
}

QList<QPair<int, QList<Hit>>> groupedByBook(const QList<Hit> &hits, const LibraryData &data)
{
    QList<QPair<int, QList<Hit>>> groups;
    QHash<int, qsizetype> indexes;
    for (const Hit &hit : hits) {
        const auto found = indexes.constFind(hit.bookId);
        if (found == indexes.cend()) {
            indexes.insert(hit.bookId, groups.size());
            groups.append({ hit.bookId, { hit } });
        } else {
            groups[*found].second.append(hit);
        }
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const auto title = [&data](int bookId) {
        const Book *book = data.book(bookId);
        return book ? (book->sort.isEmpty() ? book->title : book->sort) : QString();
    };
    std::stable_sort(groups.begin(), groups.end(), [&](const auto &a, const auto &b) {
        return collator.compare(title(a.first), title(b.first)) < 0;
    });
    for (auto &group : groups)
        group.second = sortedInBook(group.second);
    return groups;
}

QString exportText(const QList<Hit> &hits, const LibraryData &data, bool markdown, const QString &interfaceLanguage)
{
    const QString unknownChapter = CalibreLanguages::names(interfaceLanguage)->translate(QStringLiteral("Unknown chapter"));
    QStringList lines;
    QList<int> order;
    QHash<int, QList<const Hit *>> byBook;
    for (const Hit &hit : hits) {
        if (!byBook.contains(hit.bookId))
            order.append(hit.bookId);
        byBook[hit.bookId].append(&hit);
    }
    for (const int bookId : std::as_const(order)) {
        ChapterGroup root;
        root.level = 1;
        for (const Hit *hit : byBook.value(bookId))
            root.add(hit, unknownChapter);
        const Book *book = data.book(bookId);
        lines.append(QStringLiteral("# ") + (book ? book->title : QString()));
        lines.append(QString());
        root.render(&lines, markdown);
        lines.append(QString());
    }
    return lines.join(u'\n').trimmed();
}

int count(SqliteDb &db)
{
    int total = 0;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT annot_data FROM annotations"))) {
        if (parsedAnnotation(row.value(0).toByteArray()))
            ++total;
    }
    return total;
}

} // namespace AnnotationSearch
