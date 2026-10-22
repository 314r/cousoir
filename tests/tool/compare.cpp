// The comparisons of the golden tests: two copies of a library, or two
// readers, after the same changes, one made by Calibre and one by Cousoir;
// and what Calibre and Cousoir read from the same files.

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <functional>

#include "common.h"
#include "sqlite_db.h"

namespace Tool {

namespace {

// --- compare-libraries ---------------------------------------------------

// Compares two copies of a Calibre library after the same edits, one written
// by Calibre and one by Cousoir:
//
// - every table of metadata.db but the preferences Calibre updates when it
//   opens a library, with last_modified, the time of the write, left out, and
//   for books added, from id --added-from, the time and the random uuid they
//   got;
// - the files: metadata.opf as parsed XML, without the producer, which names
//   the program; cover.jpg by its size and pixels, as two JPEG encoders may
//   differ by a few bits; anything else byte for byte;
// - but the files of conversions, each book and format listed by --converted:
//   an EPUB or KEPUB by its entries, an AZW3 or MOBI by the files Calibre
//   explodes it to, without the time it was made, the random ids of the
//   anchors Calibre adds or the order of an AZW3's stylesheets and images,
//   which varies from one of Calibre's processes to the next. Their sizes in
//   the data table, which those change, are left out.
//
// Prints each difference and exits with 1 when there is one.

struct LibraryOptions
{
    bool filesOnly = false;
    std::optional<qint64> addedFrom;
    QSet<std::pair<qint64, QString>> converted;
};

// A value of a row for comparing: numbers by value, as Python compares
// them, so that 1 is 1.0.
QString valueKey(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::UnknownType:
        return QStringLiteral("N");
    case QMetaType::LongLong:
        return u'I' + QString::number(value.toLongLong());
    case QMetaType::Double: {
        const double number = value.toDouble();
        if (std::trunc(number) == number && std::abs(number) < 9.0e18)
            return u'I' + QString::number(qint64(number));
        return u'R' + QString::number(number, 'g', 17);
    }
    case QMetaType::QByteArray:
        return u'B' + QString::fromLatin1(value.toByteArray().toHex());
    default:
        return u'T' + value.toString();
    }
}

// A value as Python writes it.
QString valueText(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::UnknownType:
        return QStringLiteral("None");
    case QMetaType::LongLong:
        return QString::number(value.toLongLong());
    case QMetaType::Double: {
        QString text = QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
        if (!text.contains(u'.') && !text.contains(u'e') && !text.contains(u'n'))
            text += QStringLiteral(".0");
        return text;
    }
    case QMetaType::QByteArray:
        return QStringLiteral("b'%1'").arg(QString::fromLatin1(value.toByteArray().toHex()));
    default: {
        // In double quotes when that saves escaping, as repr() does.
        QString text = value.toString().replace(u'\\', QStringLiteral("\\\\"));
        if (text.contains(u'\'') && !text.contains(u'"'))
            return u'"' + text + u'"';
        return u'\'' + text.replace(u'\'', QStringLiteral("\\'")) + u'\'';
    }
    }
}

using Row = QList<QVariant>;

QString rowKey(const Row &row)
{
    QString key;
    for (const QVariant &value : row) {
        const QString part = valueKey(value);
        key += QString::number(part.size()) + u':' + part;
    }
    return key;
}

QString rowText(const Row &row)
{
    QStringList values;
    for (const QVariant &value : row)
        values.append(valueText(value));
    return u'(' + values.join(QStringLiteral(", ")) + (row.size() == 1 ? QStringLiteral(",)") : QStringLiteral(")"));
}

struct Table
{
    QStringList columns;
    QList<Row> rows;

    // The rows by their key, sorted, which two tables compare by.
    QStringList keys() const
    {
        QStringList found;
        for (const Row &row : rows)
            found.append(rowKey(row));
        found.sort();
        return found;
    }
};

std::optional<QMap<QString, Table>> tables(const QString &path, const LibraryOptions &options)
{
    SqliteDb db;
    QString error;
    if (!db.open(path, SqliteDb::Mode::Immutable, &error)) {
        err() << path << ": " << error << Qt::endl;
        return std::nullopt;
    }
    QStringList names;
    SqliteStatement list(db, QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"));
    while (list.step())
        names.append(list.text(0));
    QMap<QString, Table> result;
    for (const QString &name : std::as_const(names)) {
        if (name == QLatin1String("preferences") || name.startsWith(QLatin1String("annotations_fts")))
            continue;
        Table table;
        SqliteStatement columns(db, QStringLiteral("SELECT name FROM pragma_table_info(?) ORDER BY cid"));
        columns.bind(1, name);
        while (columns.step())
            table.columns.append(columns.text(0));
        const auto column = [&table](const char *name) { return table.columns.indexOf(QLatin1String(name)); };
        SqliteStatement rows(db, QStringLiteral("SELECT * FROM \"%1\"").arg(name));
        if (!rows.isValid() || rows.columnCount() != table.columns.size()) {
            err() << path << ": cannot read table " << name << ": " << rows.error() << Qt::endl;
            return std::nullopt;
        }
        while (rows.step()) {
            Row row;
            for (int i = 0; i < rows.columnCount(); ++i)
                row.append(rows.value(i));
            if (name == QLatin1String("books")) {
                row[column("last_modified")] = QStringLiteral("<now>");
                if (options.addedFrom && row[column("id")].toLongLong() >= *options.addedFrom) {
                    row[column("timestamp")] = QStringLiteral("<now>");
                    row[column("uuid")] = QStringLiteral("<uuid>");
                }
            }
            if (name == QLatin1String("data")
                && options.converted.contains({ row[column("book")].toLongLong(), row[column("format")].toString() }))
                row[column("uncompressed_size")] = QStringLiteral("<size>");
            // The row's creation time.
            if (name == QLatin1String("books_pages_link"))
                row[column("timestamp")] = QStringLiteral("<now>");
            // Calibre inserts a book's identifiers from a set, in no
            // particular order.
            if (name == QLatin1String("identifiers"))
                row[column("id")] = QStringLiteral("<id>");
            table.rows.append(row);
        }
        if (!rows.error().isEmpty()) {
            err() << path << ": cannot read table " << name << ": " << rows.error() << Qt::endl;
            return std::nullopt;
        }
        result.insert(name, table);
    }
    return result;
}

// Calibre counts a book's pages in the background; Cousoir leaves the books
// it changed for Calibre to count: either is right, on either side.
void maskPageCounts(QMap<QString, Table> &a, QMap<QString, Table> &b)
{
    const QString name = QStringLiteral("books_pages_link");
    if (!a.contains(name) || !b.contains(name))
        return;
    const qsizetype scan = b[name].columns.indexOf(QLatin1String("needs_scan"));
    if (scan < 0)
        return;
    QSet<QString> toScan;
    for (const Table *side : { &a[name], &b[name] }) {
        for (const Row &row : side->rows) {
            if (valueKey(row.value(scan)) == QLatin1String("I1"))
                toScan.insert(valueKey(row.first()));
        }
    }
    for (Table *side : { &a[name], &b[name] }) {
        for (Row &row : side->rows) {
            if (row.size() < 3 || !toScan.contains(valueKey(row.first())))
                continue;
            Row counted{ row.first() };
            for (qsizetype i = 1; i < row.size() - 2; ++i)
                counted.append(QStringLiteral("<scan>"));
            counted.append(row.at(row.size() - 2));
            counted.append(QStringLiteral("<scan>"));
            row = counted;
        }
    }
}

// The rows of `table` that `other` lacks, the first six, as Python lists
// them.
QString rowsOnlyIn(const Table &table, const Table &other)
{
    const QStringList otherKeys = other.keys();
    const QSet<QString> theirs(otherKeys.cbegin(), otherKeys.cend());
    QMap<QString, QString> only;
    for (const Row &row : table.rows) {
        const QString key = rowKey(row);
        if (!theirs.contains(key))
            only.insert(rowText(row), key);
    }
    return u'[' + only.keys().mid(0, 6).join(QStringLiteral(", ")) + u']';
}

// An element as ElementTree reads it: its tag and attributes' names as
// "{namespace}name", and its text, before its first child.
struct Element
{
    QString tag;
    QList<std::pair<QString, QString>> attributes;
    QString text;
    QList<Element> children;

    QString attribute(const QString &name) const
    {
        for (const auto &[key, value] : attributes) {
            if (key == name)
                return value;
        }
        return {};
    }

    bool hasAttribute(const QString &name) const
    {
        return std::any_of(attributes.cbegin(), attributes.cend(), [&name](const auto &a) { return a.first == name; });
    }

    const Element *child(const QString &tag) const
    {
        for (const Element &element : children) {
            if (element.tag == tag)
                return &element;
        }
        return nullptr;
    }
};

QString qualified(QStringView ns, QStringView name)
{
    return ns.isEmpty() ? name.toString() : u'{' + ns.toString() + u'}' + name.toString();
}

std::optional<Element> parseXml(const QByteArray &data)
{
    QXmlStreamReader xml(data);
    QList<Element> open;
    std::optional<Element> root;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            Element element;
            element.tag = qualified(xml.namespaceUri(), xml.name());
            for (const QXmlStreamAttribute &attribute : xml.attributes())
                element.attributes.append({ qualified(attribute.namespaceUri(), attribute.name()),
                                            attribute.value().toString() });
            open.append(element);
            break;
        }
        case QXmlStreamReader::Characters:
            if (!open.isEmpty() && open.last().children.isEmpty())
                open.last().text += xml.text();
            break;
        case QXmlStreamReader::EndElement: {
            Element element = open.takeLast();
            if (open.isEmpty())
                root = element;
            else
                open.last().children.append(element);
            break;
        }
        default:
            break;
        }
    }
    if (xml.hasError())
        return std::nullopt;
    return root;
}

const QString opfNs = QStringLiteral("{http://www.idpf.org/2007/opf}");

// A metadata.opf without what may differ: the producer, the identifiers'
// order, which follows the database's, and for a book added, its uuid and
// the time it was added.
std::optional<QStringList> opf(const QString &path, bool added)
{
    const std::optional<QByteArray> data = readFile(path);
    const std::optional<Element> root = data ? parseXml(*data) : std::nullopt;
    const Element *metadata = root ? root->child(opfNs + QLatin1String("metadata")) : nullptr;
    if (!metadata)
        return std::nullopt;
    QStringList items;
    QStringList identifiers;
    for (const Element &element : metadata->children) {
        if (element.attribute(opfNs + QLatin1String("role")) == QLatin1String("bkp"))
            continue;
        QString text = element.text.trimmed();
        QList<std::pair<QString, QString>> attributes = element.attributes;
        if (added && element.attribute(opfNs + QLatin1String("scheme")) == QLatin1String("uuid"))
            text = QStringLiteral("<uuid>");
        if (added && element.attribute(QStringLiteral("name")) == QLatin1String("calibre:timestamp")) {
            for (auto &[key, value] : attributes) {
                if (key == QLatin1String("content"))
                    value = QStringLiteral("<now>");
            }
        }
        std::sort(attributes.begin(), attributes.end());
        QStringList pairs;
        for (const auto &[key, value] : std::as_const(attributes))
            pairs.append(QStringLiteral("('%1', '%2')").arg(key, value));
        const QString item = QStringLiteral("('%1', [%2], '%3')").arg(element.tag, pairs.join(QStringLiteral(", ")), text);
        if (element.tag.endsWith(QLatin1String("identifier")) && !element.hasAttribute(QStringLiteral("id")))
            identifiers.append(item);
        else
            items.append(item);
    }
    identifiers.sort();
    items.append(identifiers);
    if (const Element *guide = root->child(opfNs + QLatin1String("guide"))) {
        for (const Element &reference : guide->children) {
            QList<std::pair<QString, QString>> attributes = reference.attributes;
            std::sort(attributes.begin(), attributes.end());
            QStringList pairs;
            for (const auto &[key, value] : std::as_const(attributes))
                pairs.append(QStringLiteral("('%1', '%2')").arg(key, value));
            items.append(QStringLiteral("guide ('%1', [%2])").arg(reference.tag, pairs.join(QStringLiteral(", "))));
        }
    }
    return items;
}

// A book folder is "Title (id)", its trash entry ".caltrash/b/id".
bool addedBook(const QString &rel, const LibraryOptions &options)
{
    if (!options.addedFrom)
        return false;
    const QStringList parts = rel.split(u'/');
    const QString folder = parts.size() >= 2 ? parts.at(parts.size() - 2) : QString();
    const QString parent = parts.size() >= 3 ? parts.at(parts.size() - 3) : QString();
    bool ok = false;
    if (parent == QLatin1String("b") && !folder.isEmpty()
        && std::all_of(folder.cbegin(), folder.cend(), [](QChar c) { return c.isDigit(); })) {
        const qint64 id = folder.toLongLong(&ok);
        return ok && id >= *options.addedFrom;
    }
    if (folder.endsWith(u')') && folder.contains(u'(')) {
        QString number = folder.mid(folder.lastIndexOf(u'(') + 1);
        number.chop(1);
        const qint64 id = number.toLongLong(&ok);
        return ok && id >= *options.addedFrom;
    }
    return false;
}

// The book and format of a book's file, "Title (id)/Title - Author.fmt".
std::optional<std::pair<qint64, QString>> bookFormat(const QString &rel)
{
    const QStringList parts = rel.split(u'/');
    const QString folder = parts.size() >= 2 ? parts.at(parts.size() - 2) : QString();
    QString number = folder.mid(folder.lastIndexOf(u'(') + 1);
    number.chop(1);
    bool ok = false;
    const qint64 book = number.toLongLong(&ok);
    if (!ok)
        return std::nullopt;
    return std::pair{ book, extension(rel).mid(1).toUpper() };
}

const char *const textEndings[] = { ".html", ".xhtml", ".htm", ".ncx", ".opf", ".css", ".xml" };

QList<QByteArray> splitLines(const QByteArray &data)
{
    QList<QByteArray> lines;
    qsizetype start = 0;
    for (qsizetype i = 0; i < data.size(); ++i) {
        if (data.at(i) != '\n' && data.at(i) != '\r')
            continue;
        lines.append(data.mid(start, i - start));
        if (data.at(i) == '\r' && i + 1 < data.size() && data.at(i + 1) == '\n')
            ++i;
        start = i + 1;
    }
    if (start < data.size())
        lines.append(data.mid(start));
    return lines;
}

// The dcterms:modified of an OPF: the time it was written.
const QRegularExpression opfModified(QStringLiteral(R"((<meta property="dcterms:modified"[^>]*>)[^<]*)"));

// The time of the conversion, the random ids Calibre gives the anchors it
// adds for a table of contents, and the names of an exploded AZW3's
// stylesheets and images, which Calibre numbers in an order that depends on
// where its process put them in memory, as it lists an element's classes.
QByteArray masked(const QString &name, QByteArray data)
{
    static const QRegularExpression id(QStringLiteral(R"(\b[0-9a-f]{32}\b)"));
    static const QRegularExpression styles(QStringLiteral(R"(styles/\d+\.css)"));
    static const QRegularExpression images(QStringLiteral(R"(images/\d+\.\w+)"));
    static const QRegularExpression classes(QStringLiteral(R"re(class="([^"]*)")re"));
    static const QRegularExpression spaces(QStringLiteral("[ \\t\\n\\r\\x0b\\x0c]+"));
    static const QRegularExpression imageId(QStringLiteral(R"(id="[^"]*")"));
    const bool isOpf = name.endsWith(QLatin1String(".opf"));
    if (isOpf)
        data = substitute(data, opfModified, QStringLiteral("\\1<now>"));
    if (std::any_of(std::begin(textEndings), std::end(textEndings),
                    [&name](const char *ending) { return name.endsWith(QLatin1String(ending)); })) {
        data = substitute(data, id, QStringLiteral("<id>"));
        data = substitute(data, styles, QStringLiteral("styles/<n>.css"));
        data = substitute(data, images, QStringLiteral("images/<n>"));
        // The classes of an element, which Calibre lists in no set order.
        const QString text = QString::fromLatin1(data);
        QString result;
        qsizetype last = 0;
        for (QRegularExpressionMatchIterator it = classes.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            QStringList names = match.captured(1).split(spaces, Qt::SkipEmptyParts);
            names.sort();
            result += text.mid(last, match.capturedStart() - last) + QStringLiteral("class=\"")
                      + names.join(u' ') + u'"';
            last = match.capturedEnd();
        }
        data = (result + text.mid(last)).toLatin1();
    }
    if (isOpf) {
        // Its manifest follows the numbers, and so do the ids of its images.
        QList<QByteArray> lines = splitLines(data);
        for (QByteArray &line : lines) {
            if (line.contains("href=\"images/<n>\""))
                line = substitute(line, imageId, QStringLiteral("id=\"<n>\""));
        }
        std::sort(lines.begin(), lines.end());
        data = lines.join('\n');
    }
    return data;
}

using Entries = QList<std::pair<QString, QString>>;

QString entriesText(const Entries &entries)
{
    QStringList items;
    for (const auto &[name, hash] : entries)
        items.append(QStringLiteral("('%1', '%2')").arg(name, hash));
    return u'[' + items.join(QStringLiteral(", ")) + u']';
}

// A zip's entries, each by the hash of what `mask` leaves of it. Calibre zips
// the files in no set order but the first.
QString zipText(const ZipEntries &zip, const std::function<QByteArray(const QString &, const QByteArray &)> &mask)
{
    Entries entries;
    for (const auto &[name, data] : zip)
        entries.append({ name, sha256(mask(name, data)) });
    if (!entries.isEmpty())
        std::sort(entries.begin() + 1, entries.end());
    return entriesText(entries);
}

// A converted file without what differs from one conversion to the next:
// an EPUB's entries, an AZW3's or MOBI's as Calibre explodes it. Nothing
// when Calibre cannot explode it.
std::optional<QString> conversion(const QString &path)
{
    const QByteArray raw = readFile(path).value_or(QByteArray());
    if (raw.startsWith("PK")) {
        if (const std::optional<ZipEntries> zip = readZip(path))
            return zipText(*zip, masked);
    }
    if (raw.mid(60, 8) == "BOOKMOBI") {
        QTemporaryDir folder;
        QProcess explode;
        explode.start(QStringLiteral("calibre-debug"), { QStringLiteral("--explode-book"), path, folder.path() });
        if (!folder.isValid() || !explode.waitForFinished(-1) || explode.exitStatus() != QProcess::NormalExit
            || explode.exitCode() != 0) {
            err() << "calibre-debug --explode-book " << path << " failed: "
                  << QString::fromUtf8(explode.readAllStandardError()) << Qt::endl;
            return std::nullopt;
        }
        static const QRegularExpression numbered(QStringLiteral(R"(^(styles|images)/\d+\.)"));
        Entries entries;
        const QMap<QString, QString> exploded = files(folder.path());
        for (auto it = exploded.cbegin(); it != exploded.cend(); ++it) {
            const QByteArray data = masked(QFileInfo(it.value()).fileName(), readFile(it.value()).value_or(QByteArray()));
            entries.append({ QString(it.key()).replace(numbered, QStringLiteral("\\1/<n>.")), sha256(data) });
        }
        std::sort(entries.begin(), entries.end());
        return entriesText(entries);
    }
    return sha256(raw);
}

// The mean difference of two images of one size, from 0 to 1, as
// ImageMagick's normalised MAE measures it.
double meanDifference(const QImage &a, const QImage &b)
{
    const QImage x = a.convertToFormat(QImage::Format_RGB32);
    const QImage y = b.convertToFormat(QImage::Format_RGB32);
    double sum = 0;
    for (int line = 0; line < x.height(); ++line) {
        const auto *p = reinterpret_cast<const QRgb *>(x.constScanLine(line));
        const auto *q = reinterpret_cast<const QRgb *>(y.constScanLine(line));
        for (int i = 0; i < x.width(); ++i) {
            sum += std::abs(qRed(p[i]) - qRed(q[i])) + std::abs(qGreen(p[i]) - qGreen(q[i]))
                   + std::abs(qBlue(p[i]) - qBlue(q[i]));
        }
    }
    return sum / (3.0 * 255.0 * double(x.width()) * double(x.height()));
}

QString sizeText(const QImage &image)
{
    return QStringLiteral("%1x%2").arg(image.width()).arg(image.height());
}

} // namespace

int compareLibraries(const QStringList &arguments)
{
    LibraryOptions options;
    QStringList paths;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QLatin1String("--files-only")) {
            options.filesOnly = true;
        } else if (argument == QLatin1String("--added-from") && i + 1 < arguments.size()) {
            options.addedFrom = arguments.at(++i).toLongLong();
        } else if (argument == QLatin1String("--converted") && i + 1 < arguments.size()) {
            for (const QString &item : arguments.at(++i).split(u',')) {
                const qsizetype colon = item.indexOf(u':');
                options.converted.insert({ item.left(colon).toLongLong(), item.mid(colon + 1).toUpper() });
            }
        } else if (!argument.startsWith(QLatin1String("--"))) {
            paths.append(argument);
        } else {
            err() << "Unknown option or missing value: " << argument << Qt::endl;
            return 2;
        }
    }
    if (paths.size() != 2) {
        err() << "Two libraries to compare are needed." << Qt::endl;
        return 2;
    }
    const QString calibreCopy = paths.at(0);
    const QString cousoirCopy = paths.at(1);
    QStringList problems;

    QMap<QString, Table> a;
    QMap<QString, Table> b;
    if (!options.filesOnly) {
        const auto calibreTables = tables(QDir(calibreCopy).filePath(QStringLiteral("metadata.db")), options);
        const auto cousoirTables = tables(QDir(cousoirCopy).filePath(QStringLiteral("metadata.db")), options);
        if (!calibreTables || !cousoirTables)
            return 1;
        a = *calibreTables;
        b = *cousoirTables;
    }
    maskPageCounts(a, b);
    QStringList names = a.keys() + b.keys();
    names.removeDuplicates();
    names.sort();
    for (const QString &name : std::as_const(names)) {
        if (!a.contains(name) || !b.contains(name)) {
            problems.append(QStringLiteral("table %1 only in one copy").arg(name));
        } else if (a[name].columns != b[name].columns || a[name].keys() != b[name].keys()) {
            problems.append(QStringLiteral("table %1 differs:\n    Calibre only: %2\n    Cousoir only: %3")
                                .arg(name, rowsOnlyIn(a[name], b[name]), rowsOnlyIn(b[name], a[name])));
        }
    }

    const auto libraryFiles = [](const QString &root) {
        QMap<QString, QString> found = files(root);
        found.removeIf([](const auto &file) {
            return file.key() == QLatin1String("metadata.db") || file.key().startsWith(QLatin1String(".calnotes"));
        });
        return found;
    };
    const QMap<QString, QString> fa = libraryFiles(calibreCopy);
    const QMap<QString, QString> fb = libraryFiles(cousoirCopy);
    QStringList rels = fa.keys() + fb.keys();
    rels.removeDuplicates();
    rels.sort();
    for (const QString &rel : std::as_const(rels)) {
        const QString name = rel.mid(rel.lastIndexOf(u'/') + 1);
        if (!fa.contains(rel) || !fb.contains(rel)) {
            problems.append(QStringLiteral("file %1 only in the %2 copy")
                                .arg(rel, fa.contains(rel) ? QStringLiteral("Calibre") : QStringLiteral("Cousoir")));
        } else if (name == QLatin1String("metadata.opf")) {
            const bool added = addedBook(rel, options);
            const std::optional<QStringList> x = opf(fa[rel], added);
            const std::optional<QStringList> y = opf(fb[rel], added);
            if (!x || !y)
                problems.append(QStringLiteral("%1 cannot be read").arg(rel));
            else if (*x != *y)
                problems.append(QStringLiteral("%1 differs:\n    Calibre: %2\n    Cousoir: %3")
                                    .arg(rel, x->join(QStringLiteral(", ")), y->join(QStringLiteral(", "))));
        } else if (name == QLatin1String("cover.jpg")) {
            if (readFile(fa[rel]) == readFile(fb[rel]))
                continue;
            const QImage x(fa[rel]);
            const QImage y(fb[rel]);
            if (x.isNull() || y.isNull()) {
                problems.append(QStringLiteral("%1 cannot be read").arg(rel));
            } else if (x.size() != y.size()) {
                problems.append(QStringLiteral("%1: %2 against %3").arg(rel, sizeText(x), sizeText(y)));
            } else if (const double mean = meanDifference(x, y); mean > 0.01) {
                problems.append(QStringLiteral("%1: covers differ by %2").arg(rel).arg(mean, 0, 'f', 4));
            }
        } else if (const auto format = bookFormat(rel); format && options.converted.contains(*format)) {
            const std::optional<QString> x = conversion(fa[rel]);
            const std::optional<QString> y = conversion(fb[rel]);
            if (!x || !y)
                problems.append(QStringLiteral("%1 cannot be read as a conversion").arg(rel));
            else if (*x != *y)
                problems.append(
                    QStringLiteral("%1 differs as a conversion:\n    Calibre: %2\n    Cousoir: %3").arg(rel, *x, *y));
        } else if (readFile(fa[rel]) != readFile(fb[rel])) {
            problems.append(QStringLiteral("%1 differs").arg(rel));
        }
    }

    for (const QString &problem : std::as_const(problems))
        out() << problem << '\n';
    out() << a.size() << " tables and " << fa.size() << " files compared, " << problems.size() << " differences\n";
    return problems.isEmpty() ? 0 : 1;
}

// --- compare-devices -----------------------------------------------------

// Compares two fake readers, Kobos or Kindles, after the same books were sent
// to or removed from them, one by Calibre and one by Cousoir:
//
// - every file but a Kobo's database, which Calibre edits and Cousoir leaves
//   alone;
// - the book files by their content, without the time Calibre stamps in
//   each: an EPUB's entries, in any order, without its dcterms:modified, a
//   PDF without its XMP MetadataDate; an APNX without the id Calibre draws;
// - metadata.calibre as parsed JSON, book by book, without the temporary
//   file each book's cover was read from, whose path Calibre keeps, or the
//   size of an EPUB, which its time changes; with --lpaths-only, only which
//   books it has;
// - driveinfo.calibre without the time of the connection, the uuid drawn for
//   a new device or the folder a Kobo is mounted on.
//
// Prints each difference and exits with 1 when there is one.

namespace {

QString content(const QString &path)
{
    static const QRegularExpression metadataDate(QStringLiteral(R"((<xmp:MetadataDate>)[^<]*)"));
    static const QRegularExpression contentGuid(QStringLiteral(R"(("contentGuid":")[0-9a-f]+)"));
    QByteArray raw = readFile(path).value_or(QByteArray());
    if (raw.startsWith("PK")) {
        if (const std::optional<ZipEntries> zip = readZip(path)) {
            return zipText(*zip, [](const QString &name, const QByteArray &data) {
                return name.endsWith(QLatin1String(".opf")) ? substitute(data, opfModified, QStringLiteral("\\1<now>"))
                                                            : data;
            });
        }
    }
    if (raw.startsWith("%PDF"))
        raw = substitute(raw, metadataDate, QStringLiteral("\\1<now>"));
    if (path.endsWith(QLatin1String(".apnx")))
        raw = substitute(raw, contentGuid, QStringLiteral("\\1<id>"));
    return sha256(raw);
}

// Whether Python takes the value for true.
bool truthy(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::Bool:
        return value.toBool();
    case QJsonValue::Double:
        return value.toDouble() != 0;
    case QJsonValue::String:
        return !value.toString().isEmpty();
    case QJsonValue::Array:
        return !value.toArray().isEmpty();
    case QJsonValue::Object:
        return !value.toObject().isEmpty();
    default:
        return false;
    }
}

std::optional<QMap<QString, QJsonObject>> deviceMetadata(const QString &path)
{
    const std::optional<QJsonValue> books = readJson(path);
    if (!books)
        return std::nullopt;
    QMap<QString, QJsonObject> found;
    for (const QJsonValue &value : books->toArray()) {
        QJsonObject book = value.toObject();
        if (truthy(book.value(QLatin1String("cover"))))
            book.insert(QStringLiteral("cover"), QStringLiteral("<cover>"));
        else
            book.insert(QStringLiteral("cover"), get(book, QStringLiteral("cover")));
        const QString lpath = book.value(QLatin1String("lpath")).toString();
        if (lpath.toLower().endsWith(QLatin1String(".epub")))
            book.insert(QStringLiteral("size"), QStringLiteral("<size>"));
        found.insert(lpath, book);
    }
    return found;
}

std::optional<QJsonObject> driveinfo(const QString &path, const QString &root)
{
    const std::optional<QJsonValue> read = readJson(path);
    if (!read)
        return std::nullopt;
    QJsonObject info = read->toObject();
    info.insert(QStringLiteral("date_last_connected"), QStringLiteral("<now>"));
    info.insert(QStringLiteral("device_store_uuid"), QStringLiteral("<uuid>"));
    if (info.contains(QLatin1String("prefix")))
        info.insert(QStringLiteral("prefix"), info.value(QLatin1String("prefix"))
                                                  .toString()
                                                  .replace(QDir(root).absolutePath(), QStringLiteral("@DEVICE@")));
    return info;
}

} // namespace

int compareDevices(const QStringList &arguments)
{
    const bool lpathsOnly = arguments.contains(QStringLiteral("--lpaths-only"));
    QStringList paths;
    for (const QString &argument : arguments) {
        if (!argument.startsWith(QLatin1String("--"))) {
            paths.append(argument);
        } else if (argument != QLatin1String("--lpaths-only")) {
            err() << "Unknown option: " << argument << Qt::endl;
            return 2;
        }
    }
    if (paths.size() != 2) {
        err() << "Two readers to compare are needed." << Qt::endl;
        return 2;
    }
    const QString calibreDevice = paths.at(0);
    const QString cousoirDevice = paths.at(1);
    QStringList problems;
    const auto deviceFiles = [](const QString &root) {
        QMap<QString, QString> found = files(root);
        found.removeIf(
            [](const auto &file) { return file.key().startsWith(QLatin1String(".kobo/KoboReader.sqlite")); });
        return found;
    };
    const QMap<QString, QString> fa = deviceFiles(calibreDevice);
    const QMap<QString, QString> fb = deviceFiles(cousoirDevice);
    QStringList rels = fa.keys() + fb.keys();
    rels.removeDuplicates();
    rels.sort();
    for (const QString &rel : std::as_const(rels)) {
        if (!fa.contains(rel) || !fb.contains(rel)) {
            problems.append(QStringLiteral("%1 only on the %2 device")
                                .arg(rel, fa.contains(rel) ? QStringLiteral("Calibre") : QStringLiteral("Cousoir")));
        } else if (rel == QLatin1String("metadata.calibre")) {
            const auto ma = deviceMetadata(fa[rel]);
            const auto mb = deviceMetadata(fb[rel]);
            if (!ma || !mb) {
                problems.append(QStringLiteral("metadata.calibre cannot be read"));
                continue;
            }
            QStringList lpaths = ma->keys() + mb->keys();
            lpaths.removeDuplicates();
            lpaths.sort();
            for (const QString &lpath : std::as_const(lpaths)) {
                if (!ma->contains(lpath) || !mb->contains(lpath)) {
                    problems.append(QStringLiteral("metadata.calibre: %1 only in %2's")
                                        .arg(lpath, ma->contains(lpath) ? QStringLiteral("Calibre")
                                                                        : QStringLiteral("Cousoir")));
                    continue;
                }
                if (lpathsOnly)
                    continue;
                const QJsonObject x = ma->value(lpath);
                const QJsonObject y = mb->value(lpath);
                QStringList keys = x.keys() + y.keys();
                keys.removeDuplicates();
                keys.sort();
                for (const QString &key : std::as_const(keys)) {
                    if (get(x, key) != get(y, key))
                        problems.append(QStringLiteral("metadata.calibre: %1: %2 differs:\n    Calibre: %3\n    Cousoir: %4")
                                            .arg(lpath, key, display(get(x, key)).left(120),
                                                 display(get(y, key)).left(120)));
                }
            }
        } else if (rel == QLatin1String("driveinfo.calibre")) {
            const auto da = driveinfo(fa[rel], calibreDevice);
            const auto db = driveinfo(fb[rel], cousoirDevice);
            if (!da || !db)
                problems.append(QStringLiteral("driveinfo.calibre cannot be read"));
            else if (*da != *db)
                problems.append(QStringLiteral("driveinfo.calibre differs:\n    Calibre: %1\n    Cousoir: %2")
                                    .arg(display(*da), display(*db)));
        } else if (content(fa[rel]) != content(fb[rel])) {
            problems.append(QStringLiteral("%1 differs").arg(rel));
        }
    }
    for (const QString &problem : std::as_const(problems))
        out() << problem << '\n';
    out() << fa.size() << " files compared, " << problems.size() << " differences\n";
    return problems.isEmpty() ? 0 : 1;
}

// --- compare-reads -------------------------------------------------------

// Compares what `cousoir-cli read` and tools/oracle/read_oracle.py read from
// the same files, field by field.

int compareReads(const QStringList &arguments)
{
    const std::optional<QJsonValue> calibreRead = readJson(arguments.at(0));
    const std::optional<QJsonValue> cousoirRead = readJson(arguments.at(1));
    if (!calibreRead || !cousoirRead)
        return 1;
    const QJsonArray calibre = calibreRead->toArray();
    const QJsonArray cousoir = cousoirRead->toArray();
    if (calibre.size() != cousoir.size()) {
        err() << calibre.size() << " books read by Calibre, " << cousoir.size() << " by Cousoir" << Qt::endl;
        return 1;
    }
    const auto normal = [](const QString &field, const QJsonValue &value) {
        if ((field == QLatin1String("series_index") || field == QLatin1String("rating")) && value.isString()) {
            bool ok = false;
            const double number = value.toString().toDouble(&ok);
            return ok ? QJsonValue(number) : value;
        }
        return value;
    };
    int differences = 0;
    QList<std::pair<QString, int>> fields;
    for (qsizetype i = 0; i < calibre.size(); ++i) {
        const QJsonObject theirs = calibre.at(i).toObject();
        const QJsonObject ours = cousoir.at(i).toObject();
        for (auto it = theirs.constBegin(); it != theirs.constEnd(); ++it) {
            const QJsonValue a = normal(it.key(), get(ours, it.key()));
            const QJsonValue b = normal(it.key(), it.value());
            if (a == b)
                continue;
            ++differences;
            auto field = std::find_if(fields.begin(), fields.end(), [&it](const auto &f) { return f.first == it.key(); });
            if (field == fields.end())
                fields.append({ it.key(), 1 });
            else
                ++field->second;
            out() << theirs.value(QLatin1String("file")).toString() << ": " << it.key() << "\n    calibre "
                  << display(b) << "\n    cousoir " << display(a) << '\n';
        }
    }
    QStringList counts;
    for (const auto &[field, count] : std::as_const(fields))
        counts.append(QStringLiteral("'%1': %2").arg(field).arg(count));
    out() << '\n' << calibre.size() << " files, " << differences << " differences";
    if (!counts.isEmpty())
        out() << " {" << counts.join(QStringLiteral(", ")) << '}';
    out() << '\n';
    return differences ? 1 : 0;
}

} // namespace Tool
