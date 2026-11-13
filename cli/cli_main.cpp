#include <QBuffer>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <optional>

#include "book_edit.h"
#include "book_import.h"
#include "calibre_db.h"
#include "calibre_job.h"
#include "calibre_list.h"
#include "calibre_search.h"
#include "calibre_template.h"
#include "commit_engine.h"
#include "concurrency_guard.h"
#include "conversion.h"
#include "device_files.h"
#include "kepub.h"
#include "kf8_writer.h"
#include "mobi_book.h"
#include "kindle.h"
#include "kindle_send.h"
#include "kobo.h"
#include "kobo_highlights.h"
#include "kobo_placement.h"
#include "kobo_sync.h"
#include "kobo_send.h"
#include "sqlite_db.h"
#include "book_text_index.h"
#include "new_library.h"
#include "save_to_disk.h"
#include "book_text_search.h"
#include "library_check.h"
#include "metadata_download.h"
#include "mtp.h"
#include "open_library.h"
#include "cousoir_paths.h"

#include "cli_commands.h"

using namespace Cli;


// The skill that tells a coding agent how to drive cousoir-cli, printed, or
// installed as Omarchy's apps install theirs (Hype, MIT, David Heinemeier
// Hansson): one copy in ~/.agents/skills, which Codex reads, linked into
// Claude Code's skills.
int skill(const QStringList &arguments)
{
    QFile bundled(QStringLiteral(":/cousoir/skill.md"));
    if (!bundled.open(QIODevice::ReadOnly)) {
        err() << QStringLiteral("The skill is missing from this build.") << Qt::endl;
        return 1;
    }
    const QByteArray text = bundled.readAll();
    if (arguments.size() == 1) {
        std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
        return 0;
    }
    if (arguments.size() != 2 || arguments.at(1) != QLatin1String("install")) {
        err() << QStringLiteral("Use “cousoir-cli skill” to print the skill, or “cousoir-cli skill install”.") << Qt::endl;
        return 2;
    }
    const QDir home = QDir::home();
    const QString folder = home.filePath(QStringLiteral(".agents/skills/cousoir"));
    QSaveFile file(folder + QStringLiteral("/SKILL.md"));
    if (!QDir().mkpath(folder) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit()) {
        err() << QStringLiteral("Could not write %1").arg(file.fileName()) << Qt::endl;
        return 1;
    }
    out() << QStringLiteral("Installed %1").arg(file.fileName()) << Qt::endl;
    if (!home.exists(QStringLiteral(".claude")))
        return 0;
    const QString link = home.filePath(QStringLiteral(".claude/skills/cousoir"));
    const QFileInfo existing(link);
    if (existing.isSymLink()) {
        QFile::remove(link);
    } else if (existing.exists()) {
        err() << QStringLiteral("%1 exists and is not a link; leaving it alone.").arg(link) << Qt::endl;
        return 1;
    }
    if (!home.mkpath(QStringLiteral(".claude/skills"))
        || !QFile::link(QStringLiteral("../../.agents/skills/cousoir"), link)) {
        err() << QStringLiteral("Could not link %1").arg(link) << Qt::endl;
        return 1;
    }
    out() << QStringLiteral("Linked %1").arg(link) << Qt::endl;
    return 0;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("cousoir"));
    app.setApplicationName(QStringLiteral("cousoir"));
    app.setApplicationVersion(QStringLiteral(COUSOIR_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Reads a Calibre library without changing it, unless told to apply changes.\n\n"
        "Commands:\n"
        "  list    Print the books as `calibredb list --for-machine --fields all` does.\n"
        "  search  Print the ids of the books an expression of Calibre's search\n"
        "          language finds, as `calibredb search` does.\n"
        "  export  Save books out of the library as Calibre's Save to disk does, with\n"
        "          its options, their files copied as they are: export FOLDER ID… or all\n"
        "  new-library  Make a new, empty Calibre library in a folder, empty or not\n"
        "          there yet, as Calibre makes one: new-library FOLDER\n"
        "  index-text  Index the text of the books into Calibre's full text index, as\n"
        "          Calibre does, turning its full text search on; formats Cousoir\n"
        "          cannot read are left to Calibre.\n"
        "  search-text  Print the books whose text has the words, by Calibre's full\n"
        "          text index and its query language: id, format and passage.\n"
        "  doctor  Report missing files, conflict copies and whether the library could be written now.\n"
        "  apply   Apply the changes of a JSON file (edits, added and removed books and\n"
        "          formats, restorations, conversions, books sent to or removed from a\n"
        "          Kobo or a Kindle), as the application makes them.\n"
        "  read    Print the metadata adding the given book files would read.\n"
        "  device  Print the books on a reader: a Kobo by the folder it is mounted on,\n"
        "          \"mtp\" or \"mtp:SERIAL\" for a Kindle connected over MTP.\n"
        "  template  Print what each template of a JSON array makes of each book.\n"
        "  download  Print what Calibre's metadata sources, on the web, find for the\n"
        "          book of the id given, and what it would change; --covers adds the\n"
        "          covers found. Without Calibre, Open Library is asked.\n"
        "  kepubify  Make a KEPUB of an EPUB, as Calibre does, without Calibre:\n"
        "          kepubify BOOK.epub BOOK.kepub\n"
        "  azw3    Make an AZW3 of an EPUB for a Kindle, as Calibre's conversion\n"
        "          does, without Calibre: azw3 BOOK.epub BOOK.azw3\n"
        "  epub    Make an EPUB of a Kindle book, AZW3 or MOBI, as Cousoir's reader\n"
        "          reads one, without Calibre: epub BOOK.azw3 BOOK.epub\n"
        "  kobo-highlights  Print the highlights and notes of a Kobo, by the folder it\n"
        "          is mounted on, or of a KoboReader.sqlite file, with the library's\n"
        "          book each is.\n"
        "  skill   Print the skill that tells a coding agent how to use cousoir-cli;\n"
        "          skill install copies it to ~/.agents/skills/cousoir and links it\n"
        "          into ~/.claude/skills.\n\n"
        "Cousoir follows Calibre's rules, ported from the code of Calibre, by Kovid\n"
        "Goyal and the Calibre contributors, https://calibre-ebook.com. GPL v3."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption libraryOption(QStringList{ QStringLiteral("library"), QStringLiteral("with-library") },
                                           QStringLiteral("The library folder; by default the one Calibre opens."),
                                           QStringLiteral("path"));
    parser.addOption(libraryOption);
    const QCommandLineOption forceOption(QStringLiteral("force"),
                                         QStringLiteral("apply, index-text: skip the sync, recent-change and conflict "
                                                        "checks; Calibre's lock still applies."));
    parser.addOption(forceOption);
    const QCommandLineOption limitOption(QStringList{ QStringLiteral("l"), QStringLiteral("limit") },
                                         QStringLiteral("search: the most ids to print."), QStringLiteral("n"),
                                         QStringLiteral("-1"));
    parser.addOption(limitOption);
    const QCommandLineOption coversOption(QStringLiteral("covers"), QStringLiteral("download: the covers too."));
    parser.addOption(coversOption);
    const QCommandLineOption jsonOption(QStringLiteral("json"),
                                        QStringLiteral("doctor, index-text, search-text: the report as JSON."));
    parser.addOption(jsonOption);
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("list, search, doctor, apply, read, device, template, download, kepubify, "
                                                "azw3, epub, kobo-highlights, export, new-library, index-text, search-text or skill"));
    parser.addPositionalArgument(QStringLiteral("edits"), QStringLiteral("apply: a JSON file of edits"), QStringLiteral("[edits]"));
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("read: book files"), QStringLiteral("[files…]"));
    parser.process(app);

    const QStringList arguments = parser.positionalArguments();
    if (arguments.value(0) == QLatin1String("read") && arguments.size() > 1)
        return read(arguments.mid(1));
    if (arguments.value(0) == QLatin1String("skill"))
        return skill(arguments);
    if (arguments.value(0) == QLatin1String("search")) {
        if (arguments.size() < 2) {
            err() << QStringLiteral("Error: You must specify the search expression") << Qt::endl;
            return 2;
        }
        const QString path = parser.isSet(libraryOption) ? parser.value(libraryOption)
                                                         : CalibreConfig::defaultLibraryPath();
        if (path.isEmpty()) {
            err() << QStringLiteral("Calibre has no library on this computer; pass --library.") << Qt::endl;
            return 2;
        }
        return search(path, arguments.mid(1).join(u' '), parser.value(limitOption).toInt());
    }
    if (arguments.value(0) == QLatin1String("export") && arguments.size() >= 3) {
        const QString path = parser.isSet(libraryOption) ? parser.value(libraryOption)
                                                         : CalibreConfig::defaultLibraryPath();
        if (path.isEmpty()) {
            err() << QStringLiteral("Calibre has no library on this computer; pass --library.") << Qt::endl;
            return 2;
        }
        return exportBooks(path, arguments.at(1), arguments.mid(2));
    }
    if (arguments.value(0) == QLatin1String("new-library") && arguments.size() == 2) {
        QString error;
        if (!NewLibrary::create(QDir::cleanPath(QFileInfo(arguments.at(1)).absoluteFilePath()), &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        out() << QStringLiteral("Made a new library in %1").arg(QFileInfo(arguments.at(1)).absoluteFilePath()) << Qt::endl;
        return 0;
    }
    if (arguments.value(0) == QLatin1String("search-text")) {
        if (arguments.size() < 2) {
            err() << QStringLiteral("Error: You must specify the words to look for") << Qt::endl;
            return 2;
        }
        const QString path = parser.isSet(libraryOption) ? parser.value(libraryOption)
                                                         : CalibreConfig::defaultLibraryPath();
        if (path.isEmpty()) {
            err() << QStringLiteral("Calibre has no library on this computer; pass --library.") << Qt::endl;
            return 2;
        }
        return searchText(path, arguments.mid(1).join(u' '), parser.value(limitOption).toInt(), parser.isSet(jsonOption));
    }
    if (arguments.value(0) == QLatin1String("kepubify") && arguments.size() == 3) {
        QString error;
        Kepub::Options options;
        if (!Kepub::kepubify(arguments.at(1), arguments.at(2), options, &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        return 0;
    }
    if (arguments.value(0) == QLatin1String("azw3") && arguments.size() == 3) {
        QString error;
        if (!Kf8::write(arguments.at(1), arguments.at(2), Kf8::Options(), &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        return 0;
    }
    if (arguments.value(0) == QLatin1String("epub") && arguments.size() == 3) {
        QString error;
        if (!MobiBook::toEpub(arguments.at(1), arguments.at(2), &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        return 0;
    }
    const bool takesOne = arguments.value(0) == QLatin1String("apply") || arguments.value(0) == QLatin1String("device")
        || arguments.value(0) == QLatin1String("template") || arguments.value(0) == QLatin1String("download")
        || arguments.value(0) == QLatin1String("kobo-highlights");
    if (arguments.isEmpty() || arguments.size() > 2 || (arguments.size() == 2) != takesOne)
        parser.showHelp(2);
    const QString libraryPath = parser.isSet(libraryOption) ? parser.value(libraryOption)
                                                            : CalibreConfig::defaultLibraryPath();
    if (libraryPath.isEmpty()) {
        err() << QStringLiteral("Calibre has no library on this computer; pass --library.") << Qt::endl;
        return 2;
    }
    if (arguments.first() == QLatin1String("list"))
        return list(libraryPath);
    if (arguments.first() == QLatin1String("doctor"))
        return doctor(libraryPath, parser.isSet(jsonOption));
    if (arguments.first() == QLatin1String("index-text"))
        return indexText(libraryPath, parser.isSet(forceOption), parser.isSet(jsonOption));
    if (arguments.first() == QLatin1String("apply"))
        return apply(libraryPath, arguments.at(1), parser.isSet(forceOption));
    if (arguments.first() == QLatin1String("device"))
        return device(libraryPath, arguments.at(1));
    if (arguments.first() == QLatin1String("template"))
        return templates(libraryPath, arguments.at(1));
    if (arguments.first() == QLatin1String("download"))
        return download(libraryPath, arguments.at(1), parser.isSet(coversOption));
    if (arguments.first() == QLatin1String("kobo-highlights"))
        return koboHighlights(libraryPath, arguments.at(1));
    err() << QStringLiteral("Unknown command “%1”.").arg(arguments.first()) << Qt::endl;
    return 2;
}
