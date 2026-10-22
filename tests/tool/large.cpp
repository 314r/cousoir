// A large library for measuring Cousoir: as many books as asked, with
// varied, made-up metadata, written into a copy of the fixture library's
// database through Calibre's SQL functions and triggers, and their folders,
// files and covers named as Calibre names them. The same arguments always
// make the same library.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QTimeZone>
#include <QUuid>

#include <cmath>
#include <filesystem>
#include <random>

#include "calibre_db.h"
#include "calibre_paths.h"
#include "calibre_text.h"
#include "common.h"
#include "sqlite_db.h"

namespace Tool {

namespace {

const char *const firstNames[] = {
    "Boris", "Marguerite", "Albert", "Simone", "Jules", "George", "Victor", "Colette", "Émile", "Gustave",
    "Honoré", "Marcel", "Annie", "Patrick", "Amélie", "Michel", "Françoise", "Romain", "Irène", "Jean",
    "Ursula", "Isaac", "Arthur", "Philip", "Margaret", "Terry", "Neil", "Octavia", "Iain", "Ann",
    "Robin", "Kim", "Liu", "Ted", "Becky", "Martha", "N. K.", "Ray", "Frank", "Dan",
    "Haruki", "Yoko", "Kazuo", "Banana", "Natsuo", "Fiodor", "Léon", "Anton", "Ivan", "Mikhaïl",
    "Gabriel", "Isabel", "Jorge Luis", "Julio", "Mario", "Italo", "Umberto", "Elena", "Primo", "Alessandro",
    "Thomas", "Hermann", "Stefan", "Franz", "Günter", "Virginia", "Jane", "Charlotte", "Emily", "Mary",
    "J. R. R.", "C. S.", "H. G.", "E. M.", "J. K.", "Zadie", "Chimamanda", "Toni", "Jesmyn", "Colson",
    "Agatha", "Dorothy", "Raymond", "Dashiell", "Georges", "Fred", "Pierre", "Maxime", "Alain", "Sylvie",
    "Hélène", "Anaïs", "Clémence", "Noël", "Zoé", "Søren", "Åsa", "Jo", "Leïla", "Joël",
};

const char *const lastNames[] = {
    "Vian", "Duras", "Camus", "de Beauvoir", "Verne", "Sand", "Hugo", "Colette", "Zola", "Flaubert",
    "de Balzac", "Proust", "Ernaux", "Modiano", "Nothomb", "Houellebecq", "Sagan", "Gary", "Némirovsky", "Giono",
    "Le Guin", "Asimov", "Clarke", "Dick", "Atwood", "Pratchett", "Gaiman", "Butler", "Banks", "Leckie",
    "Hobb", "Stanley Robinson", "Cixin", "Chiang", "Chambers", "Wells", "Jemisin", "Bradbury", "Herbert", "Simmons",
    "Murakami", "Ogawa", "Ishiguro", "Yoshimoto", "Kirino", "Dostoïevski", "Tolstoï", "Tchekhov", "Tourgueniev", "Boulgakov",
    "García Márquez", "Allende", "Borges", "Cortázar", "Vargas Llosa", "Calvino", "Eco", "Ferrante", "Levi", "Baricco",
    "Mann", "Hesse", "Zweig", "Kafka", "Grass", "Woolf", "Austen", "Brontë", "Shelley", "Tolkien",
    "Lewis", "Rowling", "Smith", "Adichie", "Morrison", "Ward", "Whitehead", "Christie", "Sayers", "Chandler",
    "Hammett", "Simenon", "Vargas", "Lemaitre", "Damasio", "Bordage", "Ayerdhal", "Werber", "Barjavel", "Pelot",
    "van der Meer", "von Trotta", "O'Brien", "MacLeod", "D'Arcy", "Saint-Exupéry", "Ørsted", "Dupont", "Martin", "Bernard",
};

// A few names in other scripts, which Calibre transliterates for folders.
const char *const otherScriptNames[] = {
    "村上 春樹", "Фёдор Достоевский", "Λέων Τολστόι", "김 영하", "نجيب محفوظ", "莫言",
};

const char *const titleStarts[] = {
    "Le", "La", "Les", "L'", "Un", "Une", "The", "A", "An", "", "", "", "", "",
};

const char *const nouns[] = {
    "Montagne", "Mer", "Écume", "Jours", "Peste", "Étranger", "Nuit", "Ville", "Forêt", "Rivière",
    "Château", "Jardin", "Silence", "Mémoire", "Horizon", "Étoile", "Chemin", "Voyage", "Cendre", "Lumière",
    "Mountain", "Sea", "Foundation", "Left Hand", "Darkness", "Dispossessed", "Dune", "Hyperion", "Fire", "Ice",
    "Winter", "Summer", "Garden", "River", "Island", "Empire", "Machine", "Library", "Clockwork", "Glass",
    "Oiseau", "Loup", "Renard", "Chat", "Baleine", "Dragon", "Sorcière", "Roi", "Reine", "Enfant",
    "Bird", "Wolf", "Fox", "Whale", "Witch", "King", "Queen", "Child", "Stranger", "Ghost",
    "Pluie", "Neige", "Vent", "Orage", "Brume", "Aube", "Crépuscule", "Saison", "Temps", "Siècle",
};

const char *const linkWords[] = {
    "de la", "des", "du", "dans la", "sous la", "of the", "in the", "and the", "et le", "sans",
};

const char *const tagNames[] = {
    "Roman", "Science-Fiction", "Fantasy", "Policier", "Thriller", "Histoire", "Biographie", "Essai", "Poésie", "Théâtre",
    "Jeunesse", "Bande dessinée", "Philosophie", "Sciences", "Voyage", "Cuisine", "Art", "Musique", "Nouvelles", "Classique",
    "Fiction", "Non-fiction", "Mystery", "Romance", "Horror", "Humour", "Space Opera", "Cyberpunk", "Steampunk", "Dystopie",
    "Uchronie", "Utopie", "Fantastique", "Gothique", "Western", "Guerre", "Politique", "Économie", "Psychologie", "Sociologie",
    "Fiction.Science Fiction", "Fiction.Fantasy", "Fiction.Mystery", "Fiction.Literary", "Non-fiction.History",
    "Non-fiction.Science", "Non-fiction.Essays", "À lire", "Lu", "Favori", "Prêté", "Club de lecture", "Prix Goncourt",
    "Prix Hugo", "Prix Nebula", "Prix Locus", "XIXe siècle", "XXe siècle", "XXIe siècle", "Japon", "Russie", "Amérique latine",
    "Italie", "Allemagne", "Angleterre", "France", "Afrique", "Nature", "Mer", "Montagne", "Espace", "Robots", "IA",
    "Magie", "Dragons", "Vampires", "Enquête", "Espionnage", "Famille", "Amour", "Amitié", "Mort", "Mémoire",
};

const char *const publisherStarts[] = {
    "Éditions", "Presses", "Le Livre de", "Folio", "Pocket", "Penguin", "Tor", "Gallimard", "Bélial'", "Actes",
    "Rivages", "Mnémos", "Atalante", "J'ai", "Points", "Orbit", "Gollancz", "Vintage", "Faber", "Harper",
};

const char *const publisherEnds[] = {
    "de Minuit", "Sud", "Noir", "Poche", "Lu", "Books", "Press", "Classics", "Imaginaire", "SF",
    "du Seuil", "Universitaires", "Modernes", "du Monde", "Nouvelles", "& Co", "Publishing", "Editions", "Verlag", "Libri",
};

const char *const seriesPatterns[] = {
    "Les Chroniques de %1", "Le Cycle de %1", "The %1 Saga", "%1", "La Trilogie de %1", "The %1 Sequence",
    "Les Enquêtes de %1", "The Book of %1", "%1 Universe", "La Quête de %1",
};

const char *const words[] = {
    "un", "une", "le", "la", "les", "des", "dans", "sur", "avec", "pour", "sans", "jamais", "toujours", "encore",
    "roman", "histoire", "monde", "temps", "homme", "femme", "enfant", "ville", "mer", "nuit", "jour", "vie",
    "mort", "amour", "guerre", "paix", "secret", "mystère", "voyage", "souvenir", "rêve", "lumière", "ombre",
    "the", "a", "of", "in", "and", "with", "story", "world", "time", "life", "death", "love", "war", "secret",
    "journey", "memory", "dream", "light", "shadow", "city", "sea", "night", "day", "family", "stranger",
    "découvre", "raconte", "explore", "traverse", "révèle", "discovers", "tells", "explores", "reveals",
    "inoubliable", "bouleversant", "magistral", "haletant", "unforgettable", "stunning", "masterful", "gripping",
};

const char *const languageCodes[] = { "fra", "fra", "fra", "fra", "eng", "eng", "eng", "deu", "spa", "ita", "jpn", "rus" };

const char *const extraFormats[] = { "MOBI", "AZW3", "PDF", "KEPUB", "TXT" };

class Generator
{
public:
    explicit Generator(quint64 seed) : m_random(seed) {}

    int below(int bound) { return int(std::uniform_int_distribution<int>(0, bound - 1)(m_random)); }
    double unit() { return std::uniform_real_distribution<double>(0, 1)(m_random); }
    bool chance(double probability) { return unit() < probability; }
    // A few items are picked far more often than the others, as a few
    // authors, tags and publishers fill most of a real library.
    int popular(int bound) { return std::min(bound - 1, int(std::pow(unit(), 2.5) * bound)); }

    template<typename T, size_t N>
    QString pick(const T (&items)[N]) { return QString::fromUtf8(items[below(int(N))]); }

    QString sentence(int count)
    {
        QStringList parts;
        for (int i = 0; i < count; ++i)
            parts.append(pick(words));
        QString text = parts.join(u' ');
        text[0] = text[0].toUpper();
        return text + u'.';
    }

    QUuid uuid()
    {
        QByteArray bytes(16, Qt::Uninitialized);
        for (char &byte : bytes)
            byte = char(below(256));
        bytes[6] = char((bytes[6] & 0x0F) | 0x40);
        bytes[8] = char((bytes[8] & 0x3F) | 0x80);
        return QUuid::fromRfc4122(bytes);
    }

private:
    std::mt19937_64 m_random;
};

struct Item
{
    QString name;
    int id = 0;
};

// Calibre's date column format: "2015-07-07 18:41:43.605980+00:00".
QString calibreDate(const QDateTime &date)
{
    return date.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz000+00:00"));
}

// Covers of a few sizes and colours, which the books' cover.jpg files link
// to: a large library's covers would otherwise fill the disk.
QStringList makeCovers(const QString &folder, Generator &generator)
{
    QStringList covers;
    QDir().mkpath(folder);
    for (int i = 0; i < 24; ++i) {
        const int width = 400 + generator.below(4) * 100;
        QImage image(width, width * 3 / 2, QImage::Format_RGB32);
        image.fill(QColor::fromHsv(generator.below(360), 80 + generator.below(150), 90 + generator.below(150)));
        QPainter painter(&image);
        for (int band = 0; band < 6; ++band) {
            painter.fillRect(0, generator.below(image.height()), image.width(), 10 + generator.below(80),
                             QColor::fromHsv(generator.below(360), 120, 200, 160));
        }
        painter.end();
        const QString path = QDir(folder).filePath(QStringLiteral("cover-%1.jpg").arg(i));
        if (!image.save(path, "JPEG", 85))
            return {};
        covers.append(path);
    }
    return covers;
}

} // namespace

int makeLibrary(const QStringList &arguments)
{
    const QString target = QDir(arguments.at(0)).absolutePath();
    bool ok = false;
    const int bookCount = arguments.at(1).toInt(&ok);
    if (!ok || bookCount < 1) {
        err() << "BOOKS must be a positive number" << Qt::endl;
        return 2;
    }
    const quint64 seed = arguments.size() > 2 ? arguments.at(2).toULongLong() : 1;
    QElapsedTimer timer;
    timer.start();

    if (QFileInfo::exists(target) && !QDir(target).isEmpty()) {
        err() << target << " is not empty" << Qt::endl;
        return 1;
    }
    QDir().mkpath(target);
    const QString database = QDir(target).filePath(QStringLiteral("metadata.db"));
    if (!QFile::copy(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library/metadata.db"), database)) {
        err() << "cannot copy the fixture's database to " << database << Qt::endl;
        return 1;
    }
    QFile::setPermissions(database, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);

    Generator generator(seed);
    const CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    const QString language = QStringLiteral("en");
    const QString unknown = QStringLiteral("Unknown");
    SqliteDb db;
    QString error;
    if (!db.open(database, SqliteDb::Mode::ReadWrite, &error)
        || !CalibreDb::prepareConnection(db, tweaks, language, &error)
        || !db.exec(QStringLiteral("PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF; BEGIN;"
                                   "DELETE FROM books; DELETE FROM authors; DELETE FROM tags; DELETE FROM series;"
                                   "DELETE FROM publishers; DELETE FROM languages; DELETE FROM ratings;"
                                   "DELETE FROM metadata_dirtied; DELETE FROM sqlite_sequence WHERE name='books';"
                                   "UPDATE library_id SET uuid='%1';")
                        .arg(generator.uuid().toString(QUuid::WithoutBraces)), &error)) {
        err() << database << ": " << error << Qt::endl;
        return 1;
    }

    // The items the books pick from, made once.
    QList<Item> authors;
    QSet<QString> authorNames;
    const int authorCount = std::max(20, bookCount / 4);
    while (authors.size() < authorCount) {
        QString name = generator.chance(0.01) ? generator.pick(otherScriptNames)
                                              : generator.pick(firstNames) + u' ' + generator.pick(lastNames);
        // Past the combinations, a middle initial tells authors apart.
        if (authorNames.contains(name.toLower()))
            name = generator.pick(firstNames) + u' ' + QChar(u'A' + generator.below(26)) + QStringLiteral(". ")
                   + generator.pick(lastNames);
        if (authorNames.contains(name.toLower()))
            continue;
        authorNames.insert(name.toLower());
        authors.append({ name });
    }
    QList<Item> tags;
    for (const char *tag : tagNames)
        tags.append({ QString::fromUtf8(tag) });
    QList<Item> series;
    QSet<QString> seriesNames;
    const int seriesCount = std::max(5, bookCount / 12);
    for (int attempts = 0; series.size() < seriesCount && attempts < seriesCount * 20; ++attempts) {
        const QString name = generator.pick(seriesPatterns).arg(generator.pick(nouns))
                             + (attempts > seriesCount * 2 ? QStringLiteral(" %1").arg(attempts) : QString());
        if (seriesNames.contains(name.toLower()))
            continue;
        seriesNames.insert(name.toLower());
        series.append({ name });
    }
    QList<Item> publishers;
    QSet<QString> publisherNames;
    for (const char *start : publisherStarts) {
        for (const char *end : publisherEnds) {
            if (generator.chance(0.4)) {
                const QString name = QString::fromUtf8(start) + u' ' + QString::fromUtf8(end);
                if (!publisherNames.contains(name.toLower())) {
                    publisherNames.insert(name.toLower());
                    publishers.append({ name });
                }
            }
        }
    }

    const auto insertItems = [&](QList<Item> &items, const QString &sql) {
        for (Item &item : items) {
            if (!db.run(sql, { item.name }, &error))
                return false;
            item.id = int(db.lastInsertRowId());
        }
        return true;
    };
    if (!insertItems(authors, QStringLiteral("INSERT INTO authors(name) VALUES(?)"))
        || !insertItems(tags, QStringLiteral("INSERT INTO tags(name) VALUES(?)"))
        || !insertItems(series, QStringLiteral("INSERT INTO series(name) VALUES(?)"))
        || !insertItems(publishers, QStringLiteral("INSERT INTO publishers(name) VALUES(?)"))) {
        err() << error << Qt::endl;
        return 1;
    }
    QHash<QString, int> languageIds;
    for (const char *code : languageCodes) {
        const QString name = QString::fromLatin1(code);
        if (!languageIds.contains(name)) {
            if (!db.run(QStringLiteral("INSERT INTO languages(lang_code) VALUES(?)"), { name }, &error)) {
                err() << error << Qt::endl;
                return 1;
            }
            languageIds.insert(name, int(db.lastInsertRowId()));
        }
    }
    QHash<int, int> ratingIds;
    for (int rating = 1; rating <= 10; ++rating) {
        if (!db.run(QStringLiteral("INSERT INTO ratings(rating) VALUES(?)"), { rating }, &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        ratingIds.insert(rating, int(db.lastInsertRowId()));
    }
    QHash<int, QString> authorSorts;
    for (const Item &author : std::as_const(authors))
        authorSorts.insert(author.id, CalibreText::authorToAuthorSort(author.name, tweaks));

    const QStringList covers = makeCovers(QDir(target).filePath(QStringLiteral(".covers")), generator);
    if (covers.isEmpty()) {
        err() << "cannot write the covers" << Qt::endl;
        return 1;
    }

    // Each book's next number in its series.
    QHash<int, double> seriesNext;
    const QDateTime start(QDate(2016, 1, 1), QTime(9, 0), QTimeZone::UTC);
    struct Previous
    {
        QString title;
        QList<int> authorIds;
    };
    QList<Previous> previous;

    for (int index = 0; index < bookCount; ++index) {
        // Now and then the same book again, as a library collects them.
        QString title;
        QList<int> bookAuthors;
        if (!previous.isEmpty() && generator.chance(0.004)) {
            const Previous &copy = previous.at(generator.below(int(previous.size())));
            title = copy.title;
            bookAuthors = copy.authorIds;
        } else {
            const QString article = generator.pick(titleStarts);
            title = article.isEmpty() ? generator.pick(nouns)
                    : article.endsWith(u'\'') ? article + generator.pick(nouns)
                                              : article + u' ' + generator.pick(nouns);
            if (generator.chance(0.6))
                title += u' ' + generator.pick(linkWords) + u' ' + generator.pick(nouns);
            if (generator.chance(0.03))
                title += QStringLiteral(" : ") + generator.sentence(8 + generator.below(20)).chopped(1);
            const int count = generator.chance(0.85) ? 1 : generator.chance(0.8) ? 2 : 3;
            while (bookAuthors.size() < count) {
                const int id = authors.at(generator.popular(int(authors.size()))).id;
                if (!bookAuthors.contains(id))
                    bookAuthors.append(id);
            }
        }
        previous.append({ title, bookAuthors });

        QStringList authorNamesOfBook;
        QStringList sorts;
        for (int id : std::as_const(bookAuthors)) {
            for (const Item &author : std::as_const(authors)) {
                if (author.id == id) {
                    authorNamesOfBook.append(author.name);
                    break;
                }
            }
            sorts.append(authorSorts.value(id));
        }

        const QDateTime added = start.addSecs(qint64(index) * (10LL * 365 * 24 * 3600 / bookCount)
                                              + generator.below(3600));
        const bool inSeries = generator.chance(0.35);
        const int seriesId = inSeries ? series.at(generator.popular(int(series.size()))).id : 0;
        const double seriesIndex = inSeries ? (seriesNext[seriesId] += 1.0) : 1.0;
        QDateTime pubdate;
        if (generator.chance(0.85))
            pubdate = QDateTime(QDate(1850 + generator.below(176), 1 + generator.below(12), 1 + generator.below(28)),
                                QTime(12, 0), QTimeZone::UTC);
        if (!db.run(QStringLiteral("INSERT INTO books(title, timestamp, pubdate, series_index, author_sort, "
                                   "last_modified) VALUES(?, ?, ?, ?, ?, ?)"),
                    { title, calibreDate(added),
                      pubdate.isValid() ? calibreDate(pubdate) : QStringLiteral("0101-01-01 00:00:00+00:00"),
                      seriesIndex, sorts.join(QStringLiteral(" & ")), calibreDate(added.addDays(generator.below(400))) },
                    &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        const int bookId = int(db.lastInsertRowId());
        const QString folder = CalibrePaths::constructPathName(bookId, title, authorNamesOfBook.first(), unknown);
        const bool hasCover = generator.chance(0.85);

        QStringList formats;
        if (generator.chance(0.92))
            formats.append(QStringLiteral("EPUB"));
        for (const char *format : extraFormats) {
            if (generator.chance(0.08))
                formats.append(QString::fromLatin1(format));
        }
        // A few books have no file, as a library of metadata only has.
        if (formats.isEmpty() && generator.chance(0.8))
            formats.append(QStringLiteral("EPUB"));
        int longest = 0;
        for (const QString &format : std::as_const(formats))
            longest = std::max(longest, int(format.size()) + 1);
        const QString fileName = CalibrePaths::constructFileName(bookId, title, authorNamesOfBook.first(), longest,
                                                                 unknown);

        QVariantList statement = { folder, hasCover ? 1 : 0, generator.uuid().toString(QUuid::WithoutBraces), bookId };
        bool written = db.run(QStringLiteral("UPDATE books SET path=?, has_cover=?, uuid=? WHERE id=?"), statement,
                              &error);
        for (int i = 0; written && i < bookAuthors.size(); ++i)
            written = db.run(QStringLiteral("INSERT INTO books_authors_link(book, author) VALUES(?, ?)"),
                             { bookId, bookAuthors.at(i) }, &error);
        QList<int> bookTags;
        const int tagCount = generator.chance(0.1) ? 0 : 1 + generator.below(5);
        while (bookTags.size() < tagCount) {
            const int id = tags.at(generator.popular(int(tags.size()))).id;
            if (!bookTags.contains(id))
                bookTags.append(id);
        }
        for (int i = 0; written && i < bookTags.size(); ++i)
            written = db.run(QStringLiteral("INSERT INTO books_tags_link(book, tag) VALUES(?, ?)"),
                             { bookId, bookTags.at(i) }, &error);
        if (written && seriesId)
            written = db.run(QStringLiteral("INSERT INTO books_series_link(book, series) VALUES(?, ?)"),
                             { bookId, seriesId }, &error);
        if (written && generator.chance(0.8))
            written = db.run(QStringLiteral("INSERT INTO books_publishers_link(book, publisher) VALUES(?, ?)"),
                             { bookId, publishers.at(generator.popular(int(publishers.size()))).id }, &error);
        if (written && generator.chance(0.5))
            written = db.run(QStringLiteral("INSERT INTO books_ratings_link(book, rating) VALUES(?, ?)"),
                             { bookId, ratingIds.value(generator.chance(0.9) ? 2 * (1 + generator.below(5))
                                                                               : 1 + 2 * generator.below(5)) },
                             &error);
        if (written && generator.chance(0.9)) {
            written = db.run(QStringLiteral("INSERT INTO books_languages_link(book, lang_code, item_order) "
                                            "VALUES(?, ?, 0)"),
                             { bookId, languageIds.value(generator.pick(languageCodes)) }, &error);
        }
        if (written && generator.chance(0.7)) {
            QString isbn = QStringLiteral("978");
            for (int i = 0; i < 10; ++i)
                isbn += QChar(u'0' + generator.below(10));
            written = db.run(QStringLiteral("INSERT INTO identifiers(book, type, val) VALUES(?, 'isbn', ?)"),
                             { bookId, isbn }, &error);
        }
        if (written && generator.chance(0.2))
            written = db.run(QStringLiteral("INSERT INTO identifiers(book, type, val) VALUES(?, 'goodreads', ?)"),
                             { bookId, QString::number(100000 + generator.below(9000000)) }, &error);
        if (written && generator.chance(0.85)) {
            QStringList paragraphs;
            const int paragraphCount = 1 + generator.below(4);
            for (int i = 0; i < paragraphCount; ++i) {
                QStringList sentences;
                const int sentenceCount = 1 + generator.below(5);
                for (int j = 0; j < sentenceCount; ++j)
                    sentences.append(generator.sentence(6 + generator.below(18)));
                paragraphs.append(QStringLiteral("<p>%1</p>").arg(sentences.join(u' ')));
            }
            written = db.run(QStringLiteral("INSERT INTO comments(book, text) VALUES(?, ?)"),
                             { bookId, QStringLiteral("<div>%1</div>").arg(paragraphs.join(QString())) }, &error);
        }

        const QString bookFolder = QDir(target).filePath(folder);
        if (written && !QDir().mkpath(bookFolder)) {
            error = QStringLiteral("cannot make %1").arg(bookFolder);
            written = false;
        }
        for (int i = 0; written && i < formats.size(); ++i) {
            const QString &format = formats.at(i);
            const qint64 size = 150000 + generator.below(4000000);
            const QString extension = format == QLatin1String("KEPUB") ? QStringLiteral("kepub") : format.toLower();
            // The file only stands for the book: its size is the database's.
            written = writeFile(QDir(bookFolder).filePath(fileName + u'.' + extension),
                                QStringLiteral("%1 %2 %3\n").arg(format, title, QString::number(bookId)).toUtf8())
                      && db.run(QStringLiteral("INSERT INTO data(book, format, uncompressed_size, name) "
                                               "VALUES(?, ?, ?, ?)"),
                                { bookId, format, size, fileName }, &error);
        }
        if (written && hasCover) {
            std::error_code code;
            std::filesystem::create_hard_link(covers.at(generator.below(int(covers.size()))).toStdString(),
                                              QDir(bookFolder).filePath(QStringLiteral("cover.jpg")).toStdString(),
                                              code);
            if (code) {
                error = QString::fromStdString(code.message());
                written = false;
            }
        }
        if (!written) {
            err() << "book " << bookId << ": " << error << Qt::endl;
            return 1;
        }
    }

    if (!db.exec(QStringLiteral("COMMIT;"), &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    out() << bookCount << " books in " << target << ", " << timer.elapsed() << " ms" << Qt::endl;
    return 0;
}

} // namespace Tool
