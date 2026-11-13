#include "book_text_index.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <functional>

#include "book_text_search.h"
#include "calibre_fts.h"
#include "calibre_text.h"
#include "commit_engine.h"
#include "epub_book.h"
#include "html_soup.h"
#include "mobi_book.h"
#include "sqlite_db.h"
#include "xml_tree.h"

namespace BookTextIndex {

namespace {

const QString xhtmlNamespace = QStringLiteral("http://www.w3.org/1999/xhtml");

// skipped_tags of db/fts/text.py.
bool skipped(const QString &name)
{
    static const QSet<QString> tags{ QStringLiteral("style"), QStringLiteral("title"), QStringLiteral("script"),
                                     QStringLiteral("head"),  QStringLiteral("img"),   QStringLiteral("svg"),
                                     QStringLiteral("math"),  QStringLiteral("rt"),    QStringLiteral("rp"),
                                     QStringLiteral("rtc") };
    return tags.contains(name);
}

// BLOCK_TAG_NAMES of ebooks/oeb/polish/utils.py.
bool block(const QString &name)
{
    static const QSet<QString> tags{
        QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"),    QStringLiteral("blockquote"),
        QStringLiteral("body"),    QStringLiteral("center"),  QStringLiteral("dd"),       QStringLiteral("dir"),
        QStringLiteral("div"),     QStringLiteral("dl"),      QStringLiteral("fieldset"), QStringLiteral("h1"),
        QStringLiteral("h2"),      QStringLiteral("h3"),      QStringLiteral("h4"),       QStringLiteral("h5"),
        QStringLiteral("h6"),      QStringLiteral("header"),  QStringLiteral("hgroup"),   QStringLiteral("isindex"),
        QStringLiteral("li"),      QStringLiteral("menu"),    QStringLiteral("noframes"), QStringLiteral("noscript"),
        QStringLiteral("ol"),      QStringLiteral("p"),       QStringLiteral("pre"),      QStringLiteral("section"),
        QStringLiteral("td"),      QStringLiteral("th"),      QStringLiteral("ul")
    };
    return tags.contains(name);
}

// tag_to_text(): the texts, each child's own after it; a block's after two
// line breaks, and a skipped element's left out, its tail kept.
void tagText(const XmlTree::Node &tag, QString *out)
{
    for (const auto &child : tag.children) {
        switch (child->kind) {
        case XmlTree::Node::Kind::Text:
        case XmlTree::Node::Kind::Cdata:
            out->append(child->text);
            break;
        case XmlTree::Node::Kind::Element: {
            const QString name = child->localName.toLower();
            if (name.isEmpty() || skipped(name))
                break;
            if (block(name))
                out->append(QStringLiteral("\n\n"));
            tagText(*child, out);
            break;
        }
        default:
            break;
        }
    }
}

// What Python's str.isspace() takes for white space.
bool pythonSpace(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x09 && u <= 0x0d) || (u >= 0x1c && u <= 0x20) || u == 0x85 || u == 0xa0 || u == 0x1680
           || (u >= 0x2000 && u <= 0x200a) || u == 0x2028 || u == 0x2029 || u == 0x202f || u == 0x205f || u == 0x3000;
}

QString pythonStrip(const QString &text)
{
    qsizetype start = 0;
    qsizetype end = text.size();
    while (start < end && pythonSpace(text.at(start)))
        ++start;
    while (end > start && pythonSpace(text.at(end - 1)))
        --end;
    return text.mid(start, end - start);
}

// html_to_text(): each body of the page, its text stripped, no more than
// two line breaks in a row.
void htmlText(XmlTree::Node *root, QStringList *texts)
{
    static const QRegularExpression breaks(QStringLiteral("\n{3,}"));
    XmlTree::Node *html = XmlTree::documentElement(root);
    if (!html)
        return;
    for (const auto &child : html->children) {
        if (child->kind != XmlTree::Node::Kind::Element || child->localName != QLatin1String("body")
            || child->namespaceUri != xhtmlNamespace)
            continue;
        QString text;
        tagText(*child, &text);
        texts->append(pythonStrip(text).replace(breaks, QStringLiteral("\n\n")));
    }
}

bool isXhtmlRoot(XmlTree::Node *root)
{
    XmlTree::Node *html = root ? XmlTree::documentElement(root) : nullptr;
    return html && html->localName == QLatin1String("html") && html->namespaceUri == xhtmlNamespace;
}

// A page of the spine as the container's parsed() reads it: as XML when it
// is XHTML, else as tag soup; another XML document as XML alone, its bodies
// none; anything else not at all.
void pageText(const QByteArray &data, const QString &mediaType, QStringList *texts)
{
    static const QSet<QString> documents{ QStringLiteral("application/xhtml+xml"), QStringLiteral("text/html"),
                                          QStringLiteral("text/x-oeb1-document"),
                                          QStringLiteral("application/x-dtbook+xml") };
    const QString type = mediaType.toLower();
    if (documents.contains(type)) {
        std::unique_ptr<XmlTree::Node> root = XmlTree::parse(data);
        if (!isXhtmlRoot(root.get())) {
            QString text = XmlTree::xmlText(data);
            text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(u'\r', u'\n');
            root = HtmlSoup::parse(text);
        }
        if (root)
            htmlText(root.get(), texts);
        return;
    }
    if (type.contains(QLatin1String("xml"))) {
        std::unique_ptr<XmlTree::Node> root = XmlTree::parse(data);
        if (isXhtmlRoot(root.get()))
            htmlText(root.get(), texts);
    }
}

Extraction epubText(const QString &path)
{
    QString error;
    const std::unique_ptr<EpubBook> book = EpubBook::open(path, &error);
    if (!book)
        return { true, {}, error };
    QStringList texts;
    for (const EpubBook::SpineItem &item : book->spine()) {
        if (book->contains(item.name))
            pageText(book->read(item.name), book->mediaType(item.name), &texts);
    }
    return { true, texts.join(QStringLiteral("\n\n\n")), {} };
}

// pdftotext(): Calibre's own run of Poppler's, its output with no ASCII
// control characters; nothing when it fails.
Extraction pdfText(const QString &path)
{
    const QString program = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    if (program.isEmpty())
        return {};
    QProcess process;
    process.setWorkingDirectory(QFileInfo(path).absolutePath());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, { QStringLiteral("-enc"), QStringLiteral("UTF-8"), QStringLiteral("-nodiag"),
                             QStringLiteral("-eol"), QStringLiteral("unix"), QFileInfo(path).fileName(),
                             QStringLiteral("-") });
    if (!process.waitForStarted())
        return {};
    // Calibre gives a worker 30 minutes.
    if (!process.waitForFinished(30 * 60 * 1000)) {
        process.kill();
        process.waitForFinished();
        return { true, {}, QObject::tr("Extracting text from the PDF file took too long") };
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return { true, {}, {} };
    return { true, CalibreText::cleanAsciiChars(QString::fromUtf8(process.readAllStandardOutput())), {} };
}

QString indexPathOf(const QString &libraryPath)
{
    return BookTextSearch::indexPath(libraryPath);
}

bool openIndex(SqliteDb &db, const QString &libraryPath, const QString &interfaceLanguage, QString *error)
{
    // A search reading it is waited for.
    return db.open(indexPathOf(libraryPath), SqliteDb::Mode::ReadWrite, error)
           && db.exec(QStringLiteral("PRAGMA busy_timeout=5000"), error)
           && CalibreFts::registerTokenizers(db.handle(), interfaceLanguage, error);
}

// Statements run in one transaction, rolled back when one fails.
bool transaction(SqliteDb &db, const std::function<bool()> &work, QString *error)
{
    if (!db.exec(QStringLiteral("BEGIN IMMEDIATE"), error))
        return false;
    if (work() && db.exec(QStringLiteral("COMMIT"), error))
        return true;
    db.exec(QStringLiteral("ROLLBACK"), nullptr);
    return false;
}

// Python's len() of a text: its code points.
qint64 codePoints(const QString &text)
{
    qint64 count = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (!text.at(i).isLowSurrogate())
            ++count;
    }
    return count;
}

} // namespace

Extraction extract(const QString &path, const QString &format)
{
    const QString upper = format.toUpper();
    Extraction extraction;
    if (upper == QLatin1String("PDF")) {
        extraction = pdfText(path);
    } else if (upper == QLatin1String("EPUB") || upper == QLatin1String("KEPUB")) {
        extraction = epubText(path);
    } else if (upper == QLatin1String("AZW3") || upper == QLatin1String("AZW") || upper == QLatin1String("MOBI")) {
        // A book Cousoir cannot make an EPUB of, an old MOBI or one with
        // DRM, is left to Calibre.
        QTemporaryDir folder;
        const QString epub = folder.filePath(QStringLiteral("book.epub"));
        QString error;
        if (!folder.isValid() || !MobiBook::toEpub(path, epub, &error, MobiBook::OldPages::Whole))
            return {};
        extraction = epubText(epub);
    } else {
        return {};
    }
    // unicodedata.normalize('NFC', ans).replace('­', '').
    extraction.text = extraction.text.normalized(QString::NormalizationForm_C).remove(QChar(0x00ad));
    return extraction;
}

bool trackFormats(SqliteDb &db, QString *error)
{
    // fts_triggers.sql, recorded rather than done: the index follows once
    // the database is published.
    return db.exec(QStringLiteral(
                       "CREATE TEMP TABLE IF NOT EXISTS cousoir_formats(kind INTEGER, book INTEGER, format TEXT);"
                       "DELETE FROM temp.cousoir_formats;"
                       "CREATE TEMP TRIGGER IF NOT EXISTS cousoir_fts_book_deleted AFTER DELETE ON main.books BEGIN"
                       " INSERT INTO temp.cousoir_formats VALUES (0, OLD.id, NULL); END;"
                       "CREATE TEMP TRIGGER IF NOT EXISTS cousoir_fts_format_deleted AFTER DELETE ON main.data BEGIN"
                       " INSERT INTO temp.cousoir_formats VALUES (1, OLD.book, OLD.format); END;"
                       "CREATE TEMP TRIGGER IF NOT EXISTS cousoir_fts_format_added AFTER INSERT ON main.data BEGIN"
                       " INSERT INTO temp.cousoir_formats VALUES (2, NEW.book, NEW.format); END;"
                       "CREATE TEMP TRIGGER IF NOT EXISTS cousoir_fts_format_updated AFTER UPDATE ON main.data BEGIN"
                       " INSERT INTO temp.cousoir_formats VALUES (2, NEW.book, NEW.format); END;"),
                   error);
}

QList<FormatChange> formatChanges(SqliteDb &db)
{
    QList<FormatChange> changes;
    for (const QVariantList &row :
         db.rows(QStringLiteral("SELECT kind, book, format FROM temp.cousoir_formats ORDER BY rowid"))) {
        FormatChange change;
        change.kind = static_cast<FormatChange::Kind>(row.value(0).toInt());
        change.book = row.value(1).toInt();
        change.format = row.value(2).toString();
        changes.append(change);
    }
    return changes;
}

bool enabled(SqliteDb &db)
{
    return db.value(QStringLiteral("SELECT val FROM main.preferences WHERE key='fts_enabled'")).toString()
           == QLatin1String("true");
}

bool follow(const QString &libraryPath, const QList<FormatChange> &changes, const QString &interfaceLanguage,
            QString *error)
{
    if (changes.isEmpty() || !QFileInfo::exists(indexPathOf(libraryPath)))
        return true;
    SqliteDb db;
    if (!openIndex(db, libraryPath, interfaceLanguage, error))
        return false;
    return transaction(
        db,
        [&] {
            for (const FormatChange &change : changes) {
                bool done = true;
                switch (change.kind) {
                case FormatChange::Kind::BookRemoved:
                    done = db.run(QStringLiteral("DELETE FROM books_text WHERE book=?"), { change.book }, error)
                           && db.run(QStringLiteral("DELETE FROM dirtied_formats WHERE book=?"), { change.book }, error);
                    break;
                case FormatChange::Kind::FormatRemoved:
                    done = db.run(QStringLiteral("DELETE FROM books_text WHERE book=? AND format=?"),
                                  { change.book, change.format }, error)
                           && db.run(QStringLiteral("DELETE FROM dirtied_formats WHERE book=? AND format=?"),
                                     { change.book, change.format }, error);
                    break;
                case FormatChange::Kind::FormatChanged:
                    done = db.run(QStringLiteral("INSERT OR IGNORE INTO dirtied_formats(book, format) VALUES (?, ?)"),
                                  { change.book, change.format }, error);
                    break;
                }
                if (!done)
                    return false;
            }
            return true;
        },
        error);
}

bool create(const QString &libraryPath, const QList<Job> &formats, const QString &interfaceLanguage, QString *error)
{
    const QString path = indexPathOf(libraryPath);
    if (QFileInfo::exists(path)) {
        if (error)
            *error = QObject::tr("The library already has a full text index.");
        return false;
    }
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
    }
    SqliteDb db;
    // fts_sqlite.sql, as SchemaUpgrade runs it on a new index, then
    // dirty_existing().
    const bool made = openIndex(db, libraryPath, interfaceLanguage, error) && transaction(
        db,
        [&] {
            if (!db.exec(QStringLiteral(
                    "CREATE TABLE dirtied_formats ( id INTEGER PRIMARY KEY,\n"
                    "\tbook INTEGER NOT NULL,\n"
                    "\tformat TEXT NOT NULL COLLATE NOCASE,\n"
                    "    in_progress INTEGER NOT NULL DEFAULT FALSE,\n"
                    "    UNIQUE(book, format)\n"
                    ");\n"
                    "CREATE TABLE books_text ( id INTEGER PRIMARY KEY,\n"
                    "\tbook INTEGER NOT NULL,\n"
                    "\ttimestamp REAL NOT NULL,\n"
                    "\tformat TEXT NOT NULL COLLATE NOCASE,\n"
                    "    format_hash TEXT NOT NULL COLLATE NOCASE,\n"
                    "    format_size INTEGER NOT NULL DEFAULT 0,\n"
                    "    searchable_text TEXT NOT NULL DEFAULT '',\n"
                    "    text_size INTEGER NOT NULL DEFAULT 0,\n"
                    "    text_hash TEXT NOT NULL COLLATE NOCASE DEFAULT '',\n"
                    "    err_msg TEXT DEFAULT '',\n"
                    "    UNIQUE(book, format)\n"
                    ");\n"
                    "CREATE VIRTUAL TABLE books_fts USING fts5(searchable_text, content = 'books_text', "
                    "content_rowid = 'id', tokenize = 'calibre remove_diacritics 2');\n"
                    "CREATE VIRTUAL TABLE books_fts_stemmed USING fts5(searchable_text, content = 'books_text', "
                    "content_rowid = 'id', tokenize = 'porter calibre remove_diacritics 2');\n"
                    "CREATE TRIGGER books_fts_insert_trg AFTER INSERT ON books_text \n"
                    "BEGIN\n"
                    "    INSERT INTO books_fts(rowid, searchable_text) VALUES (NEW.id, NEW.searchable_text);\n"
                    "    INSERT INTO books_fts_stemmed(rowid, searchable_text) VALUES (NEW.id, NEW.searchable_text);\n"
                    "    DELETE FROM dirtied_formats WHERE book=NEW.book AND format=NEW.format;\n"
                    "END;\n"
                    "CREATE TRIGGER books_fts_delete_trg AFTER DELETE ON books_text \n"
                    "BEGIN\n"
                    "    INSERT INTO books_fts(books_fts, rowid, searchable_text) VALUES('delete', OLD.id, "
                    "OLD.searchable_text);\n"
                    "    INSERT INTO books_fts_stemmed(books_fts_stemmed, rowid, searchable_text) VALUES('delete', "
                    "OLD.id, OLD.searchable_text);\n"
                    "END;\n"
                    "CREATE TRIGGER books_fts_update_trg AFTER UPDATE ON books_text \n"
                    "BEGIN\n"
                    "    INSERT INTO books_fts(books_fts, rowid, searchable_text) VALUES('delete', OLD.id, "
                    "OLD.searchable_text);\n"
                    "    INSERT INTO books_fts(rowid, searchable_text) VALUES (NEW.id, NEW.searchable_text);\n"
                    "    INSERT INTO books_fts_stemmed(books_fts_stemmed, rowid, searchable_text) VALUES('delete', "
                    "OLD.id, OLD.searchable_text);\n"
                    "    INSERT INTO books_fts_stemmed(rowid, searchable_text) VALUES (NEW.id, NEW.searchable_text);\n"
                    "    DELETE FROM dirtied_formats WHERE book=NEW.book AND format=NEW.format;\n"
                    "END;\n"
                    "PRAGMA user_version=1;"),
                         error))
                return false;
            for (const Job &job : formats) {
                if (!db.run(QStringLiteral("INSERT OR IGNORE INTO dirtied_formats(book, format) VALUES (?, ?)"),
                            { job.book, job.format.toUpper() }, error))
                    return false;
            }
            return true;
        },
        error);
    db.close();
    if (!made)
        QFile::remove(path);
    return made;
}

QList<Job> waiting(const QString &libraryPath, QString *error)
{
    QList<Job> jobs;
    if (!QFileInfo::exists(indexPathOf(libraryPath)))
        return jobs;
    SqliteDb db;
    if (!db.open(indexPathOf(libraryPath), SqliteDb::Mode::ReadOnly, error))
        return jobs;
    // A format Calibre was indexing when it stopped is waiting again, as its
    // next start makes it.
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT book, format FROM dirtied_formats ORDER BY id")))
        jobs.append({ row.value(0).toInt(), row.value(1).toString().toUpper() });
    return jobs;
}

bool hashFile(const QString &path, qint64 *size, QString *hash)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash sha1(QCryptographicHash::Sha1);
    qint64 total = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1 << 20);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return false;
        total += chunk.size();
        sha1.addData(chunk);
    }
    *size = total;
    *hash = QString::fromLatin1(sha1.result().toHex());
    return true;
}

bool current(const QString &libraryPath, const Job &job, qint64 size, const QString &hash)
{
    SqliteDb db;
    return db.open(indexPathOf(libraryPath), SqliteDb::Mode::ReadOnly, nullptr)
           && !db.rows(QStringLiteral("SELECT id FROM books_text WHERE book=? AND format=? AND format_size=? "
                                      "AND format_hash=?"),
                       { job.book, job.format.toUpper(), size, hash })
                   .isEmpty();
}

bool store(const QString &libraryPath, const QList<Result> &results, const QString &interfaceLanguage, QString *error)
{
    if (results.isEmpty())
        return true;
    SqliteDb db;
    if (!openIndex(db, libraryPath, interfaceLanguage, error))
        return false;
    return transaction(
        db,
        [&] {
            for (const Result &result : results) {
                const QString format = result.format.toUpper();
                // commit_result(): the same text again is not stored again.
                QString text = result.text;
                QString textHash;
                if (!text.isEmpty()) {
                    textHash = QString::fromLatin1(
                        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
                    if (!db.rows(QStringLiteral("SELECT id FROM books_text WHERE book=? AND format=? AND text_hash=?"),
                                 { result.book, format, textHash })
                             .isEmpty())
                        text.clear();
                }
                // add_text(): seconds since 1970 in UTC.
                const double timestamp = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
                // INSERT OR REPLACE, the row replaced taken out of the
                // index first, as its delete trigger does.
                const auto replace = [&] {
                    return db.run(QStringLiteral("DELETE FROM books_text WHERE book=? AND format=?"),
                                  { result.book, format }, error);
                };
                bool done = true;
                if (!result.errorMessage.isEmpty()) {
                    done = replace()
                           && db.run(QStringLiteral("INSERT INTO books_text (book, timestamp, format, format_size, "
                                                    "format_hash, err_msg) VALUES (?, ?, ?, ?, ?, ?)"),
                                     { result.book, timestamp, format, result.formatSize, result.formatHash,
                                       result.errorMessage },
                                     error);
                } else if (!text.isEmpty()) {
                    done = replace()
                           && db.run(QStringLiteral("INSERT INTO books_text (book, timestamp, format, format_size, "
                                                    "format_hash, searchable_text, text_size, text_hash) VALUES "
                                                    "(?, ?, ?, ?, ?, ?, ?, ?)"),
                                     { result.book, timestamp, format, result.formatSize, result.formatHash, text,
                                       codePoints(text), textHash },
                                     error);
                } else {
                    done = db.run(QStringLiteral("DELETE FROM dirtied_formats WHERE book=? AND format=?"),
                                  { result.book, format }, error);
                }
                if (!done)
                    return false;
            }
            return true;
        },
        error);
}

bool reads(const QString &format)
{
    static const QSet<QString> formats{ QStringLiteral("EPUB"), QStringLiteral("KEPUB"), QStringLiteral("AZW3"),
                                        QStringLiteral("AZW"), QStringLiteral("MOBI") };
    const QString upper = format.toUpper();
    if (upper == QLatin1String("PDF"))
        return !QStandardPaths::findExecutable(QStringLiteral("pdftotext")).isEmpty();
    return formats.contains(upper);
}

bool calibreReads(const QString &format)
{
    // is_fmt_extractable() of db/fts/text.py, for Calibre 9: its input
    // formats but comics, and archives.
    static const QSet<QString> formats{
        QStringLiteral("AZW"),   QStringLiteral("AZW3"),     QStringLiteral("AZW4"),  QStringLiteral("CHM"),
        QStringLiteral("DJV"),   QStringLiteral("DJVU"),     QStringLiteral("DOCM"),  QStringLiteral("DOCX"),
        QStringLiteral("DOWNLOADED_RECIPE"),                 QStringLiteral("EPUB"),  QStringLiteral("FB2"),
        QStringLiteral("FBZ"),   QStringLiteral("HTM"),      QStringLiteral("HTML"),  QStringLiteral("HTMLZ"),
        QStringLiteral("KEPUB"), QStringLiteral("LIT"),      QStringLiteral("LRF"),   QStringLiteral("MARKDOWN"),
        QStringLiteral("MD"),    QStringLiteral("MOBI"),     QStringLiteral("ODT"),   QStringLiteral("OPF"),
        QStringLiteral("PDB"),   QStringLiteral("PDF"),      QStringLiteral("PML"),   QStringLiteral("PMLZ"),
        QStringLiteral("POBI"),  QStringLiteral("PRC"),      QStringLiteral("RB"),    QStringLiteral("RECIPE"),
        QStringLiteral("RTF"),   QStringLiteral("SHTM"),     QStringLiteral("SHTML"), QStringLiteral("SNB"),
        QStringLiteral("TCR"),   QStringLiteral("TEXT"),     QStringLiteral("TEXTILE"), QStringLiteral("TXT"),
        QStringLiteral("TXTZ"),  QStringLiteral("UPDB"),     QStringLiteral("XHTM"),  QStringLiteral("XHTML"),
        QStringLiteral("ZIP"),   QStringLiteral("RAR"),      QStringLiteral("OEBZIP")
    };
    return formats.contains(format.toUpper());
}

Run index(const QString &libraryPath, const QString &interfaceLanguage, const QList<Task> &tasks,
          const std::atomic_bool *cancel, const std::function<void(int)> &progress)
{
    Run run;
    QList<Result> batch;
    int done = 0;
    QElapsedTimer clock;
    clock.start();
    const auto flush = [&] {
        if (batch.isEmpty())
            return;
        bool stored = false;
        QString error;
        if (!CommitEngine::withLibraryLock([&] { stored = store(libraryPath, batch, interfaceLanguage, &error); }))
            run.calibreRunning = true;
        else if (stored)
            run.stored += int(batch.size());
        else
            qWarning("The books' text could not be kept: %s", qPrintable(error));
        batch.clear();
        clock.restart();
    };
    for (const Task &task : tasks) {
        if ((cancel && *cancel) || run.calibreRunning)
            break;
        const QString format = task.format.toUpper();
        // A format Calibre reads and Cousoir does not waits for Calibre.
        if (!task.path.isEmpty() && calibreReads(format) && !reads(format)) {
            ++done;
            continue;
        }
        Result result{ task.book, format, 0, {}, {}, {} };
        qint64 size = 0;
        QString hash;
        // A file gone, or one Calibre does not read, no longer waits; nor
        // one whose text is there already.
        const bool readable = !task.path.isEmpty() && calibreReads(format) && hashFile(task.path, &size, &hash);
        if (readable && !current(libraryPath, { task.book, format }, size, hash)) {
            const Extraction extraction = extract(task.path, format);
            if (!extraction.supported) {
                run.leftToCalibre.insert({ task.book, format });
                ++done;
                continue;
            }
            result.formatSize = size;
            result.formatHash = hash;
            result.text = extraction.text;
            result.errorMessage = extraction.error;
        }
        batch.append(result);
        ++done;
        if (batch.size() >= 20 || clock.elapsed() > 2000) {
            flush();
            if (progress)
                progress(done);
        }
    }
    if (!(cancel && *cancel) && !run.calibreRunning)
        flush();
    if (progress)
        progress(done);
    return run;
}

} // namespace BookTextIndex
