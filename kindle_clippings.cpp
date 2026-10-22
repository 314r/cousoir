#include "kindle_clippings.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace KindleClippings {

namespace {

const QByteArray separator = "==========";

// The words of the Kindle's languages for each kind, in lower case.
Clipping::Kind kindOf(const QString &description)
{
    const QString text = description.toLower();
    const auto any = [&text](std::initializer_list<const char16_t *> words) {
        return std::any_of(words.begin(), words.end(), [&text](const char16_t *word) {
            return text.contains(QStringView(word));
        });
    };
    if (any({ u"bookmark", u"signet", u"lesezeichen", u"marcador", u"segnalibro", u"bladwijzer", u"закладка",
              u"ブックマーク", u"书签" }))
        return Clipping::Kind::Bookmark;
    if (any({ u"highlight", u"surlignement", u"markierung", u"subrayado", u"evidenziazione", u"destaque",
              u"markering", u"выделение", u"ハイライト", u"标注" }))
        return Clipping::Kind::Highlight;
    if (any({ u"note", u"notiz", u"nota", u"notitie", u"заметка", u"メモ", u"笔记" }))
        return Clipping::Kind::Note;
    return Clipping::Kind::Unknown;
}

// "170-72" is 170 to 172 on older Kindles.
int rangeEnd(const QString &start, const QString &end)
{
    if (end.isEmpty())
        return start.toInt();
    if (end.size() < start.size())
        return (start.left(start.size() - end.size()) + end).toInt();
    return end.toInt();
}

// "Pratchett, Terry;Gaiman, Neil" as names and their sort.
void readAuthors(const QString &written, QStringList *authors, QString *authorSort)
{
    QStringList names;
    for (const QString &name : written.split(u';', Qt::SkipEmptyParts)) {
        if (!name.trimmed().isEmpty())
            names.append(name.trimmed());
    }
    const bool sorted = !names.isEmpty() && std::all_of(names.cbegin(), names.cend(), [](const QString &name) {
        return name.count(u',') == 1;
    });
    if (!sorted) {
        *authors = names;
        return;
    }
    *authorSort = names.join(QStringLiteral(" & "));
    for (const QString &name : std::as_const(names))
        authors->append((name.section(u',', 1).trimmed() + u' ' + name.section(u',', 0, 0).trimmed()).trimmed());
}

QString position(const Clipping &clipping)
{
    QStringList parts;
    if (clipping.page > 0)
        parts.append(QObject::tr("page %1").arg(clipping.page));
    if (clipping.start > 0) {
        parts.append(clipping.end > clipping.start ? QObject::tr("location %1-%2").arg(clipping.start).arg(clipping.end)
                                                   : QObject::tr("location %1").arg(clipping.start));
    }
    return parts.join(QStringLiteral(", "));
}

} // namespace

QString readDate(const QString &raw)
{
    const QString text = raw.toLower();
    static const QList<QStringList> months{
        { QStringLiteral("january"), QStringLiteral("janvier"), QStringLiteral("januar"), QStringLiteral("enero"),
          QStringLiteral("gennaio"), QStringLiteral("janeiro"), QStringLiteral("januari") },
        { QStringLiteral("february"), QStringLiteral("février"), QStringLiteral("februar"), QStringLiteral("febrero"),
          QStringLiteral("febbraio"), QStringLiteral("fevereiro"), QStringLiteral("februari") },
        { QStringLiteral("march"), QStringLiteral("mars"), QStringLiteral("märz"), QStringLiteral("marzo"),
          QStringLiteral("março"), QStringLiteral("maart") },
        { QStringLiteral("april"), QStringLiteral("avril"), QStringLiteral("abril"), QStringLiteral("aprile") },
        { QStringLiteral("may"), QStringLiteral("mai"), QStringLiteral("mayo"), QStringLiteral("maggio"),
          QStringLiteral("maio"), QStringLiteral("mei") },
        { QStringLiteral("june"), QStringLiteral("juin"), QStringLiteral("juni"), QStringLiteral("junio"),
          QStringLiteral("giugno"), QStringLiteral("junho") },
        { QStringLiteral("july"), QStringLiteral("juillet"), QStringLiteral("juli"), QStringLiteral("julio"),
          QStringLiteral("luglio"), QStringLiteral("julho") },
        { QStringLiteral("august"), QStringLiteral("août"), QStringLiteral("agosto"), QStringLiteral("augustus") },
        { QStringLiteral("september"), QStringLiteral("septembre"), QStringLiteral("septiembre"),
          QStringLiteral("settembre"), QStringLiteral("setembro") },
        { QStringLiteral("october"), QStringLiteral("octobre"), QStringLiteral("oktober"), QStringLiteral("octubre"),
          QStringLiteral("ottobre"), QStringLiteral("outubro") },
        { QStringLiteral("november"), QStringLiteral("novembre"), QStringLiteral("noviembre"), QStringLiteral("novembro") },
        { QStringLiteral("december"), QStringLiteral("décembre"), QStringLiteral("dezember"), QStringLiteral("diciembre"),
          QStringLiteral("dicembre"), QStringLiteral("dezembro") },
    };
    int month = 0;
    qsizetype monthAt = -1;
    for (int i = 0; i < months.size() && month == 0; ++i) {
        for (const QString &name : months.at(i)) {
            static const QRegularExpression letters(QStringLiteral("[^\\W\\d_]+"), QRegularExpression::UseUnicodePropertiesOption);
            for (auto it = letters.globalMatch(text); it.hasNext();) {
                const QRegularExpressionMatch word = it.next();
                if (word.captured(0) == name) {
                    month = i + 1;
                    monthAt = word.capturedStart();
                    break;
                }
            }
            if (month)
                break;
        }
    }
    static const QRegularExpression yearPattern(QStringLiteral("\\b(\\d{4})\\b"));
    static const QRegularExpression timePattern(QStringLiteral("\\b(\\d{1,2}):(\\d{2})(?::(\\d{2}))?\\s*(am|pm)?"));
    static const QRegularExpression dayPattern(QStringLiteral("\\b(\\d{1,2})\\b\\.?"));
    const QRegularExpressionMatch year = yearPattern.match(text);
    if (!month || !year.hasMatch())
        return {};
    // The day: the number nearest the month that is no part of the time.
    const QRegularExpressionMatch time = timePattern.match(text);
    int day = 0;
    qsizetype distance = text.size();
    for (auto it = dayPattern.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (time.hasMatch() && match.capturedStart() >= time.capturedStart() && match.capturedStart() < time.capturedEnd())
            continue;
        const qsizetype away = std::abs(match.capturedStart() - monthAt);
        if (away < distance) {
            distance = away;
            day = match.captured(1).toInt();
        }
    }
    QTime clock(0, 0);
    if (time.hasMatch()) {
        int hour = time.captured(1).toInt();
        if (time.captured(4) == QLatin1String("pm") && hour < 12)
            hour += 12;
        else if (time.captured(4) == QLatin1String("am") && hour == 12)
            hour = 0;
        clock = QTime(hour, time.captured(2).toInt(), time.captured(3).toInt());
    }
    const QDate date(year.captured(1).toInt(), month, day);
    if (!date.isValid() || !clock.isValid())
        return {};
    // The Kindle's own time, without a zone.
    return QDateTime(date, clock).toString(Qt::ISODate);
}

QList<Clipping> parse(const QByteArray &file)
{
    QList<Clipping> clippings;
    QString text = QString::fromUtf8(file);
    text.remove(QChar(0xfeff));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    static const QRegularExpression pagePattern(
        QStringLiteral("(?:page|seite|página|pagina|bladzijde|страница|стр\\.)\\s*(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression locationPattern(
        QStringLiteral("(?:location|loc\\.|emplacement|position|posición|posizione|posição|locatie|позиция|位置no\\.)\\s*"
                       "(\\d+)(?:\\s*-\\s*(\\d+))?"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &entry : text.split(QString::fromLatin1(separator))) {
        QStringList lines = entry.split(u'\n');
        while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
            lines.removeFirst();
        if (lines.size() < 2)
            continue;
        Clipping clipping;
        // "Title (Authors)": the last parentheses, a title may have others.
        const QString heading = lines.takeFirst().trimmed();
        static const QRegularExpression authorsPattern(QStringLiteral("^(.*\\S)\\s*\\(([^()]*)\\)$"));
        if (const QRegularExpressionMatch match = authorsPattern.match(heading); match.hasMatch()) {
            clipping.title = match.captured(1).trimmed();
            clipping.authors = match.captured(2).trimmed();
        } else {
            clipping.title = heading;
        }
        const QString description = lines.takeFirst().trimmed();
        const QStringList parts = description.split(u'|');
        clipping.kind = kindOf(parts.first());
        if (const QRegularExpressionMatch page = pagePattern.match(description); page.hasMatch())
            clipping.page = page.captured(1).toInt();
        if (const QRegularExpressionMatch location = locationPattern.match(description); location.hasMatch()) {
            clipping.start = location.captured(1).toInt();
            clipping.end = rangeEnd(location.captured(1), location.captured(2));
        }
        if (parts.size() > 1) {
            const QString added = parts.last().trimmed();
            const QString date = readDate(added);
            clipping.added = date.isEmpty() ? added : date;
        }
        clipping.text = lines.join(u'\n').trimmed();
        clippings.append(clipping);
    }
    return clippings;
}

QList<Book> byBook(const QList<Clipping> &clippings)
{
    QList<Book> books;
    QHash<QString, qsizetype> index;
    QList<QList<Clipping>> grouped;
    for (const Clipping &clipping : clippings) {
        const QString key = clipping.title + u'\x1f' + clipping.authors;
        if (!index.contains(key)) {
            index.insert(key, books.size());
            Book book;
            book.title = clipping.title;
            readAuthors(clipping.authors, &book.authors, &book.authorSort);
            books.append(book);
            grouped.append(QList<Clipping>{});
        }
        grouped[index.value(key)].append(clipping);
    }
    for (qsizetype b = 0; b < books.size(); ++b) {
        QList<Clipping> highlights;
        QList<Clipping> notes;
        QList<Clipping> bookmarks;
        for (const Clipping &clipping : std::as_const(grouped.at(b))) {
            if (clipping.kind == Clipping::Kind::Highlight && !clipping.text.isEmpty()) {
                // A highlight changed: the later version, over the same
                // place, holds the earlier one's text or is held in it.
                bool replaced = false;
                for (Clipping &earlier : highlights) {
                    const bool overlap = clipping.start > 0 ? clipping.start <= earlier.end && earlier.start <= clipping.end
                                                            : clipping.page == earlier.page;
                    if (overlap && (clipping.text.contains(earlier.text) || earlier.text.contains(clipping.text))) {
                        earlier = clipping;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    highlights.append(clipping);
            } else if (clipping.kind == Clipping::Kind::Note && !clipping.text.isEmpty()) {
                notes.append(clipping);
            } else if (clipping.kind == Clipping::Kind::Bookmark) {
                if (std::none_of(bookmarks.cbegin(), bookmarks.cend(), [&clipping](const Clipping &bookmark) {
                        return bookmark.start == clipping.start && bookmark.page == clipping.page;
                    }))
                    bookmarks.append(clipping);
            }
        }
        struct Placed
        {
            ReaderNote note;
            int order;
        };
        QList<Placed> placed;
        QList<bool> noteUsed(notes.size(), false);
        for (const Clipping &highlight : std::as_const(highlights)) {
            ReaderNote note;
            note.type = QStringLiteral("Highlight");
            note.text = highlight.text;
            note.created = highlight.added;
            note.position = position(highlight);
            note.location = highlight.start > 0 ? highlight.start : highlight.page;
            // The note made on it, at its end; the last one made.
            for (qsizetype n = notes.size() - 1; n >= 0; --n) {
                const Clipping &made = notes.at(n);
                const bool on = highlight.start > 0 ? made.start >= highlight.start && made.start <= highlight.end
                                                    : made.page == highlight.page && made.start == 0;
                if (!noteUsed.at(n) && on) {
                    note.type = QStringLiteral("Annotation");
                    note.annotation = made.text;
                    noteUsed[n] = true;
                    break;
                }
            }
            placed.append({ note, note.location });
        }
        for (qsizetype n = 0; n < notes.size(); ++n) {
            if (noteUsed.at(n))
                continue;
            ReaderNote note;
            note.type = QStringLiteral("Annotation");
            note.annotation = notes.at(n).text;
            note.created = notes.at(n).added;
            note.position = position(notes.at(n));
            note.location = notes.at(n).start > 0 ? notes.at(n).start : notes.at(n).page;
            placed.append({ note, note.location });
        }
        for (const Clipping &bookmark : std::as_const(bookmarks)) {
            ReaderNote note;
            note.type = QStringLiteral("Bookmark");
            note.created = bookmark.added;
            note.position = position(bookmark);
            note.location = bookmark.start > 0 ? bookmark.start : bookmark.page;
            placed.append({ note, note.location });
        }
        std::stable_sort(placed.begin(), placed.end(), [](const Placed &a, const Placed &b) { return a.order < b.order; });
        for (const Placed &item : std::as_const(placed))
            books[b].notes.append(item.note);
    }
    books.removeIf([](const Book &book) { return book.notes.isEmpty(); });
    return books;
}

} // namespace KindleClippings
