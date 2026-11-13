// BookLibrary: editing books, alone or together, downloads, and undo.

#include "book_library.h"
#include "book_library_support.h"

using namespace LibrarySupport;

bool BookLibrary::readerAvailable() const
{
#ifdef COUSOIR_READER
    // Set up before the application, as main() does.
    return BookReader::ready();
#else
    return false;
#endif
}

bool BookLibrary::writable() const
{
    return m_writable;
}

void BookLibrary::setWritable(bool writable)
{
    if (m_writable == writable)
        return;
    m_writable = writable;
    emit writableChanged();
    startTextIndexing();
}

bool BookLibrary::saving() const
{
    return m_commitWatcher.isRunning() || !m_pending.isEmpty();
}

bool BookLibrary::canRevert() const
{
    return m_originalBookId != 0 && m_originalBookId == m_currentBookId;
}

QString BookLibrary::publishedKey() const
{
    return m_publishedKey;
}

QString BookLibrary::calibreLanguage() const
{
    return m_calibreLanguage;
}

void BookLibrary::setCalibreLanguage(const QString &language)
{
    if (m_calibreLanguage == language.trimmed())
        return;
    m_calibreLanguage = language.trimmed();
    emit calibreLanguageChanged();
}

QString BookLibrary::interfaceLanguage() const
{
    return effectiveConfig().interfaceLanguage;
}

QString BookLibrary::backupFolder() const
{
    return CousoirPaths::backupFolder(m_libraryPath);
}

CalibreConfig BookLibrary::effectiveConfig() const
{
    CalibreConfig config = m_config;
    if (!m_calibreLanguage.isEmpty())
        config.interfaceLanguage = m_calibreLanguage;
    return config;
}

QVariantList BookLibrary::customFields(const Book &book) const
{
    QVariantList fields;
    for (qsizetype index = 0; index < m_data.customColumns.size(); ++index) {
        const CustomColumn &column = m_data.customColumns.at(index);
        const QJsonObject display = QJsonDocument::fromJson(column.display).object();
        const bool names = display.value(QLatin1String("is_names")).toBool();
        const QString interpretAs = display.value(QLatin1String("interpret_as")).toString();
        const bool html = column.datatype == QLatin1String("comments") && interpretAs != QLatin1String("short-text")
            && interpretAs != QLatin1String("long-text") && interpretAs != QLatin1String("markdown");
        const CustomValue value = book.custom.value(index);
        const QString &type = column.datatype;
        // What the page shows, and what the field edits.
        QString text;
        QString editText;
        if (type == QLatin1String("composite")) {
            // As its template made it.
            text = value.value.toString();
        } else if (column.multiple) {
            text = value.value.toStringList().join(names ? QStringLiteral(" & ") : QStringLiteral(", "));
            editText = text;
        } else if (value.value.isNull()) {
        } else if (type == QLatin1String("rating")) {
            text = BookDisplay::rating(value.value.toInt());
            editText = value.value.toString();
        } else if (type == QLatin1String("bool")) {
            text = value.value.toBool() ? tr("Yes") : tr("No");
            editText = value.value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        } else if (type == QLatin1String("datetime")) {
            // Shown in the language's order, edited as an ISO date, which
            // every language types alike.
            text = BookDisplay::date(value.value.toDateTime());
            editText = CalibreDb::isUndefinedDate(value.value.toDateTime())
                ? QString() : value.value.toDateTime().toLocalTime().date().toString(Qt::ISODate);
        } else if (type == QLatin1String("float")) {
            text = PythonJson::floatRepr(value.value.toDouble());
            editText = text;
        } else if (type == QLatin1String("series")) {
            text = tr("%1 [%2]").arg(value.value.toString(), CalibreText::seriesIndexText(value.seriesIndex));
            editText = value.value.toString();
        } else if (type == QLatin1String("comments")) {
            // HTML unless the column says it holds plain text or Markdown.
            text = html ? Description::toText(value.value.toString()) : value.value.toString();
            editText = text;
        } else {
            text = value.value.toString();
            editText = text;
        }
        QStringList choices;
        for (const QJsonValue &choice : display.value(QLatin1String("enum_values")).toArray())
            choices.append(choice.toString());
        fields.append(QVariantMap{
            { QStringLiteral("key"), u'#' + column.label },
            { QStringLiteral("name"), column.name },
            { QStringLiteral("datatype"), type },
            { QStringLiteral("multiple"), column.multiple },
            { QStringLiteral("names"), names },
            { QStringLiteral("editable"), column.editable && type != QLatin1String("composite") },
            { QStringLiteral("halfStars"), display.value(QLatin1String("allow_half_stars")).toBool() },
            { QStringLiteral("html"), html },
            // A long text holding more than its text shows, a table or an
            // image, which saving from its text would leave out.
            { QStringLiteral("losesFormatting"), type == QLatin1String("comments") && html
                                                     && Description::losesFormatting(value.value.toString()) },
            { QStringLiteral("choices"), choices },
            { QStringLiteral("text"), text },
            { QStringLiteral("editText"), editText },
            { QStringLiteral("seriesIndexText"), value.value.isNull() ? QString()
                                                                      : CalibreText::seriesIndexText(value.seriesIndex) },
        });
    }
    return fields;
}

QVariantMap BookLibrary::editableValues(const Book &book) const
{
    QStringList identifiers;
    for (const auto &identifier : book.identifiers)
        identifiers.append(identifier.first + u':' + identifier.second);
    QVariantMap values{
        { QStringLiteral("title"), book.title },
        { QStringLiteral("sort"), book.sort },
        { QStringLiteral("authors"), book.authors },
        { QStringLiteral("authorSort"), book.authorSort },
        { QStringLiteral("series"), book.series },
        { QStringLiteral("seriesIndex"), book.seriesIndex },
        { QStringLiteral("tags"), book.tags },
        { QStringLiteral("rating"), book.rating },
        { QStringLiteral("publisher"), book.publisher },
        { QStringLiteral("languages"), book.languages },
        { QStringLiteral("identifiers"), identifiers.join(QStringLiteral(", ")) },
        // The exact date, which a revert puts back as it was.
        { QStringLiteral("pubdate"), CalibreDb::isUndefinedDate(book.pubdate) ? QString()
                                                                              : CalibreDb::isoFormat(book.pubdate) },
        { QStringLiteral("comments"), book.comments },
    };
    // The custom columns, as updateBook() takes them back.
    for (qsizetype index = 0; index < m_data.customColumns.size(); ++index) {
        const CustomColumn &column = m_data.customColumns.at(index);
        if (column.datatype == QLatin1String("composite"))
            continue;
        const CustomValue value = book.custom.value(index);
        const QString key = u'#' + column.label;
        // A date as its exact text, the undefined date as nothing, which
        // Calibre's adapter makes undefined again.
        if (column.datatype == QLatin1String("datetime") && !value.value.isNull())
            values.insert(key, CalibreDb::isUndefinedDate(value.value.toDateTime())
                                   ? QString() : CalibreDb::isoFormat(value.value.toDateTime()));
        else
            values.insert(key, value.value);
        if (column.datatype == QLatin1String("series") && !value.value.isNull())
            values.insert(key + QStringLiteral("_index"), value.seriesIndex);
    }
    return values;
}

QString BookLibrary::textToHtml(const QString &text) const
{
    return Description::toHtml(text);
}

QVariantList BookLibrary::paletteSearch(const QString &text, const QVariantList &commands, int limit) const
{
    // A command's key rides, by its index, in the hit's id while it is
    // matched.
    QList<Palette::Hit> given;
    QStringList keys;
    for (const QVariant &command : commands) {
        const QVariantMap map = command.toMap();
        given.append({ map.value(QStringLiteral("kind")).toString(), int(keys.size()),
                       map.value(QStringLiteral("title")).toString(), map.value(QStringLiteral("detail")).toString(), 0 });
        keys.append(map.value(QStringLiteral("key")).toString());
    }
    QVariantList hits;
    for (const Palette::Hit &hit : Palette::search(m_index.search, text, limit, given)) {
        const bool command = hit.kind == QLatin1String("action") || hit.kind == QLatin1String("section");
        hits.append(QVariantMap{
            { QStringLiteral("kind"), hit.kind },
            { QStringLiteral("id"), command ? 0 : hit.id },
            { QStringLiteral("key"), command ? keys.value(hit.id) : QString() },
            { QStringLiteral("title"), hit.title },
            { QStringLiteral("detail"), hit.detail },
        });
    }
    return hits;
}

bool BookLibrary::isDate(const QString &text) const
{
    return text.trimmed().isEmpty() || FieldWriters::parseOnlyDate(text, QDateTime::currentDateTimeUtc())
        || FieldWriters::parseExactDate(text);
}

bool BookLibrary::isCustomDate(const QString &text) const
{
    return text.trimmed().isEmpty() || FieldWriters::parseCustomDate(text);
}

void BookLibrary::openBackupFolder()
{
    QDir().mkpath(backupFolder());
    QDesktopServices::openUrl(QUrl::fromLocalFile(backupFolder()));
}

QVariantMap BookLibrary::downloadedChanges(int bookId, const MetadataDownload::Found &found, const QStringList &fields,
                                           bool appendComments) const
{
    const Book *book = findBook(bookId);
    return book ? MetadataDownload::changes(m_data, *book, effectiveConfig(), found, fields, appendComments) : QVariantMap();
}

QVariantList BookLibrary::downloadFields(int bookId, const MetadataDownload::Found &found, const QStringList &ignored,
                                         bool appendComments) const
{
    const Book *book = findBook(bookId);
    if (!book)
        return {};
    const CalibreConfig config = effectiveConfig();
    const QVariantMap current = editableValues(*book);
    // Each field as the dialog shows it, from the values updateBook() takes.
    const auto text = [&config](const QString &field, const QVariantMap &values) -> QString {
        const QVariant value = values.value(field);
        if (field == QLatin1String("authors"))
            return value.toStringList().join(QStringLiteral(" & "));
        if (field == QLatin1String("tags"))
            return value.toStringList().join(QStringLiteral(", "));
        if (field == QLatin1String("rating"))
            return BookDisplay::rating(value.toInt());
        if (field == QLatin1String("languages")) {
            QStringList names;
            for (const QString &code : value.toStringList())
                names.append(CalibreText::languageName(code, config.interfaceLanguage));
            return names.join(QStringLiteral(", "));
        }
        if (field == QLatin1String("identifiers")) {
            QStringList pairs;
            if (value.typeId() == QMetaType::QVariantMap) {
                const QVariantMap map = value.toMap();
                for (auto it = map.cbegin(); it != map.cend(); ++it)
                    pairs.append(it.key() + u':' + it.value().toString());
            } else {
                pairs = value.toString().split(QStringLiteral(", "), Qt::SkipEmptyParts);
            }
            pairs.sort();
            return pairs.join(QStringLiteral(", "));
        }
        if (field == QLatin1String("pubdate")) {
            const QDateTime date = QDateTime::fromString(value.toString(), Qt::ISODate);
            return date.isValid() ? date.toLocalTime().date().toString(Qt::ISODate) : QString();
        }
        if (field == QLatin1String("series")) {
            const QString series = value.toString();
            return series.isEmpty() ? QString()
                                    : tr("%1, book %2").arg(series, CalibreText::seriesIndexText(values.value(QStringLiteral("seriesIndex")).toDouble()));
        }
        if (field == QLatin1String("comments"))
            return Description::toText(value.toString()).trimmed();
        return value.toString();
    };
    static const QHash<QString, const char *> labels{
        { QStringLiteral("title"), QT_TR_NOOP("Title") },         { QStringLiteral("authors"), QT_TR_NOOP("Authors") },
        { QStringLiteral("rating"), QT_TR_NOOP("Rating") },       { QStringLiteral("publisher"), QT_TR_NOOP("Publisher") },
        { QStringLiteral("tags"), QT_TR_NOOP("Tags") },           { QStringLiteral("identifiers"), QT_TR_NOOP("Identifiers") },
        { QStringLiteral("pubdate"), QT_TR_NOOP("Published") },   { QStringLiteral("series"), QT_TR_NOOP("Series") },
        { QStringLiteral("languages"), QT_TR_NOOP("Languages") }, { QStringLiteral("comments"), QT_TR_NOOP("Description") },
    };
    QVariantList rows;
    for (const QString &field : MetadataDownload::fields()) {
        const QVariantMap changes = MetadataDownload::changes(m_data, *book, config, found, { field }, appendComments);
        if (changes.isEmpty())
            continue;
        QVariantMap after = current;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            after.insert(it.key(), it.value());
        const QString before = text(field, current);
        const QString now = text(field, after);
        rows.append(QVariantMap{
            { QStringLiteral("key"), field },
            { QStringLiteral("label"), tr(labels.value(field)) },
            { QStringLiteral("current"), before },
            { QStringLiteral("found"), now },
            // Calibre's preferences may leave a field aside.
            { QStringLiteral("chosen"), before != now && !ignored.contains(field) },
        });
    }
    return rows;
}

QString BookLibrary::updateBook(int bookId, const QVariantMap &changes)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    BookEdit edit;
    const QString error = makeEdit(*book, changes, &edit);
    if (!error.isEmpty())
        return error;
    keepOriginal(*book);
    return queueEdit(edit);
}

QString BookLibrary::makeEdit(const Book &book, const QVariantMap &changes, BookEdit *edit, QStringList *fields) const
{
    if (changes.contains(QStringLiteral("pubdate")) && !isDate(changes.value(QStringLiteral("pubdate")).toString()))
        return tr("“%1” is not a date.").arg(changes.value(QStringLiteral("pubdate")).toString());

    QVariantMap adjusted = changes;
    const CalibreConfig config = effectiveConfig();
    // Calibre's editor: a sort that followed its title or authors keeps
    // following them, any other stays as it is.
    if (adjusted.contains(QStringLiteral("title")) && !adjusted.contains(QStringLiteral("sort"))) {
        const QString language = book.languages.value(0);
        const QString automatic = CalibreText::titleSort(book.title, language, config.tweaks, config.interfaceLanguage);
        adjusted.insert(QStringLiteral("sort"), book.sort == automatic
            ? CalibreText::titleSort(adjusted.value(QStringLiteral("title")).toString(), language, config.tweaks,
                                     config.interfaceLanguage)
            : book.sort);
    }
    if (adjusted.contains(QStringLiteral("authors")) && !adjusted.contains(QStringLiteral("authorSort"))
        && !LibraryCheck::authorSortFollows(m_data, book))
        adjusted.insert(QStringLiteral("authorSort"), book.authorSort);

    QString error;
    if (!BookEdit::fromVariantMap(book.id, adjusted, edit, &error))
        return error;
    if (fields) {
        for (auto it = adjusted.cbegin(); it != adjusted.cend(); ++it) {
            if (it.key() != QLatin1String("coverFile") && it.key() != QLatin1String("removeCover"))
                fields->append(it.key());
        }
        // The author sort follows the authors by itself.
        if (adjusted.contains(QStringLiteral("authors")) && !fields->contains(QStringLiteral("authorSort")))
            fields->append(QStringLiteral("authorSort"));
    }
    return {};
}

void BookLibrary::keepOriginal(const Book &book)
{
    if (book.id != m_currentBookId || m_originalBookId == book.id)
        return;
    m_originalBookId = book.id;
    m_original = editableValues(book);
    m_originalCover = coverBytes(book);
    emit canRevertChanged();
}

QString BookLibrary::saveDescription(int bookId, const QString &text)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    // Unchanged, the description keeps its HTML as it is.
    if (text.trimmed() == Description::toText(book->comments).trimmed())
        return {};
    return updateBook(bookId, { { QStringLiteral("comments"), Description::toHtml(text) } });
}

QString BookLibrary::setCover(int bookId, const QString &fileUrl)
{
    const QString path = fileUrl.startsWith(QLatin1String("file:")) ? QUrl(fileUrl).toLocalFile() : fileUrl;
    if (QImageReader(path).format().isEmpty())
        return tr("%1 is not an image Cousoir can read.").arg(QFileInfo(path).fileName());
    return updateBook(bookId, { { QStringLiteral("coverFile"), path } });
}

QString BookLibrary::pasteCover(int bookId)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    const QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull())
        return tr("The clipboard holds no image.");
    BookEdit edit;
    edit.bookId = bookId;
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    edit.cover = data;
    keepOriginal(*book);
    return queueEdit(edit);
}

QString BookLibrary::removeCover(int bookId)
{
    return updateBook(bookId, { { QStringLiteral("removeCover"), true } });
}

void BookLibrary::revertCurrentBook()
{
    const Book *book = findBook(m_currentBookId);
    if (!canRevert() || !book)
        return;
    const QVariantMap current = editableValues(*book);
    QVariantMap changes;
    for (auto it = m_original.constBegin(); it != m_original.constEnd(); ++it) {
        if (current.value(it.key()) != it.value())
            changes.insert(it.key(), it.value());
    }
    BookEdit edit;
    QString error;
    BookEdit::fromVariantMap(m_currentBookId, changes, &edit, &error);
    if (coverBytes(*book) != m_originalCover)
        edit.cover = m_originalCover;
    m_originalBookId = 0;
    m_original.clear();
    m_originalCover.clear();
    emit canRevertChanged();
    if (!edit.isEmpty())
        queueEdit(edit);
}

int BookLibrary::bulkUndoCount() const
{
    return int(m_bulkUndo.size());
}

QString BookLibrary::updateBooks(const QVariantList &bookIds, const QVariantMap &changes)
{
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    if (changes.contains(QStringLiteral("pubdate")) && !isDate(changes.value(QStringLiteral("pubdate")).toString()))
        return tr("“%1” is not a date.").arg(changes.value(QStringLiteral("pubdate")).toString());
    QVariantMap withBooks = changes;
    withBooks.insert(QStringLiteral("books"), bookIds);
    BulkEdit bulk;
    QString error;
    if (!BulkEdit::fromVariantMap(withBooks, &bulk, &error))
        return error;
    bulk.bookIds.removeIf([this](int id) { return !findBook(id); });
    if (bulk.isEmpty())
        return {};
    // The undo of an earlier one gives way; this one's comes once it is
    // saved.
    if (!m_bulkUndo.isEmpty()) {
        clearBulkUndo();
        emit bulkUndoChanged();
    }
    CommitEngine::Request request;
    request.bulkEdits.append(bulk);
    return queueChange(request);
}

void BookLibrary::undoBulkEdit()
{
    if (m_bulkUndo.isEmpty() || !m_writable)
        return;
    CommitEngine::Request request;
    for (auto it = m_bulkUndo.constBegin(); it != m_bulkUndo.constEnd(); ++it) {
        const Book *book = findBook(it.key());
        if (!book)
            continue;
        // A field changed since, by Cousoir or by Calibre, stays as it is.
        const QVariantMap current = editableValues(*book);
        QVariantMap changes;
        for (auto field = it->before.constBegin(); field != it->before.constEnd(); ++field) {
            if (current.value(field.key()) == it->after.value(field.key()) && field.value() != it->after.value(field.key()))
                changes.insert(field.key(), field.value());
        }
        // The cover as it was, read now from where it was kept.
        if (it->coverBefore) {
            if (it->coverBefore->isEmpty())
                changes.insert(QStringLiteral("removeCover"), true);
            else
                changes.insert(QStringLiteral("coverFile"), *it->coverBefore);
        }
        BookEdit edit;
        QString error;
        if (BookEdit::fromVariantMap(it.key(), changes, &edit, &error) && !edit.isEmpty())
            request.edits.append(edit);
    }
    clearBulkUndo();
    emit bulkUndoChanged();
    if (!request.edits.isEmpty())
        queueChange(request);
}

void BookLibrary::keepBulkUndo(const QList<BulkEdit> &bulkEdits, const QList<Book> &before)
{
    // The fields the edits set.
    QStringList fields;
    for (const BulkEdit &bulk : bulkEdits) {
        if (bulk.authors)
            fields << QStringLiteral("authors") << QStringLiteral("authorSort");
        if (bulk.series)
            fields << QStringLiteral("series") << QStringLiteral("seriesIndex");
        if (bulk.removeAllTags || !bulk.addTags.isEmpty() || !bulk.removeTags.isEmpty())
            fields << QStringLiteral("tags");
        if (bulk.rating)
            fields << QStringLiteral("rating");
        if (bulk.publisher)
            fields << QStringLiteral("publisher");
        if (bulk.languages)
            fields << QStringLiteral("languages");
        if (bulk.pubdate)
            fields << QStringLiteral("pubdate");
    }
    keepUndo(fields, before, {});
}

void BookLibrary::keepUndo(const QStringList &fields, const QList<Book> &before, const QHash<int, QString> &covers)
{
    clearBulkUndo();
    for (const Book &old : before) {
        const Book *now = findBook(old.id);
        if (!now)
            continue;
        const QVariantMap was = editableValues(old);
        const QVariantMap is = editableValues(*now);
        BulkUndo undo;
        for (const QString &field : fields) {
            undo.before.insert(field, was.value(field));
            undo.after.insert(field, is.value(field));
        }
        if (covers.contains(old.id))
            undo.coverBefore = covers.value(old.id);
        if (undo.before != undo.after || undo.coverBefore)
            m_bulkUndo.insert(old.id, undo);
    }
    emit bulkUndoChanged();
}

void BookLibrary::clearBulkUndo()
{
    for (const BulkUndo &undo : std::as_const(m_bulkUndo)) {
        if (undo.coverBefore && !undo.coverBefore->isEmpty())
            QFile::remove(*undo.coverBefore);
    }
    m_bulkUndo.clear();
}

void BookLibrary::dropUndoPlans()
{
    for (const UndoPlan &plan : std::as_const(m_undoPlans)) {
        for (const QString &cover : plan.covers) {
            if (!cover.isEmpty())
                QFile::remove(cover);
        }
    }
    m_undoPlans.clear();
}

QString BookLibrary::undoCoverFolder() const
{
    return QDir(cacheFolder(m_libraryPath)).filePath(QStringLiteral("undo-covers"));
}

QString BookLibrary::updateEachBook(const QList<QPair<int, QVariantMap>> &changes)
{
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    CommitEngine::Request request;
    UndoPlan plan;
    QHash<int, QByteArray> oldCovers;
    for (const auto &[bookId, bookChanges] : changes) {
        const Book *book = findBook(bookId);
        if (!book)
            continue;
        BookEdit edit;
        QStringList fields;
        const QString error = makeEdit(*book, bookChanges, &edit, &fields);
        if (!error.isEmpty())
            return error;
        if (edit.isEmpty())
            continue;
        request.edits.append(edit);
        plan.before.append(*book);
        for (const QString &field : std::as_const(fields)) {
            if (!plan.fields.contains(field))
                plan.fields.append(field);
        }
        if (edit.cover)
            oldCovers.insert(bookId, coverBytes(*book));
    }
    if (request.edits.isEmpty())
        return {};
    // The undo of an earlier change gives way; this one's comes once it is
    // saved, with the covers it replaces kept in the cache till then.
    if (!m_bulkUndo.isEmpty()) {
        clearBulkUndo();
        emit bulkUndoChanged();
    }
    if (!oldCovers.isEmpty()) {
        const QDir folder(undoCoverFolder());
        folder.mkpath(QStringLiteral("."));
        for (auto it = oldCovers.cbegin(); it != oldCovers.cend(); ++it) {
            QString kept;
            if (!it.value().isEmpty()) {
                kept = folder.filePath(QStringLiteral("%1-%2").arg(m_lastChangeId + 1).arg(it.key()));
                QSaveFile file(kept);
                if (!file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size() || !file.commit())
                    return tr("Cannot keep the covers to put back in %1.").arg(folder.path());
            }
            plan.covers.insert(it.key(), kept);
        }
    }
    const QString error = queueChange(request);
    if (error.isEmpty())
        m_undoPlans.insert(m_lastChangeId, plan);
    return error;
}

QString BookLibrary::queueEdit(const BookEdit &edit)
{
    if (edit.isEmpty())
        return {};
    // With the edits just before it, if the last change waiting is edits.
    if (!m_pending.isEmpty() && !m_pending.constLast().hasOperations() && m_pending.constLast().bulkEdits.isEmpty()
        && !m_undoPlans.contains(m_pending.constLast().id)) {
        m_pending.last().edits.append(edit);
    } else {
        CommitEngine::Request request;
        request.id = ++m_lastChangeId;
        request.edits.append(edit);
        m_pending.append(request);
    }
    emit savingChanged();
    startCommit();
    return {};
}
