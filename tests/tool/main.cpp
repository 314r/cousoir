// cousoir-test-tool: what the tests run besides Cousoir and Calibre. The
// golden tests' comparisons and the steps of their fake readers, the book
// files of the fixtures, and the fake sending job the unit tests run in
// place of Calibre's.

#include <QCoreApplication>
#include <QFileInfo>

#include "common.h"

namespace {

struct Command
{
    const char *name;
    int (*run)(const QStringList &);
    int minimum;
    int maximum;
    const char *usage;
};

const Command commands[] = {
    { "compare-libraries", Tool::compareLibraries, 2, 7,
      "[--files-only] [--added-from N] [--converted BOOK:FORMAT,...] CALIBRE COUSOIR" },
    { "compare-devices", Tool::compareDevices, 2, 3, "[--lpaths-only] CALIBRE COUSOIR" },
    { "compare-reads", Tool::compareReads, 2, 2, "CALIBRE.json COUSOIR.json" },
    { "conversion-settings", Tool::conversionSettings, 1, 1, "LIBRARY" },
    { "kobo-import", Tool::koboImport, 1, 1, "KOBO" },
    { "kindle-sync", Tool::kindleSync, 1, 1, "KINDLE" },
    { "run-sql", Tool::runSql, 2, 2, "DATABASE FILE.sql" },
    { "device-lpath", Tool::deviceLpath, 2, 2, "METADATA.CALIBRE BOOK" },
    { "device-books", Tool::deviceBooks, 0, 0, "< LISTING.json" },
    { "check-matches", Tool::checkMatches, 1, -1, "MATCHES.json..." },
    { "pick-conversions", Tool::pickConversions, 2, 2, "METADATA.DB OUT.json" },
    { "pick-sends", Tool::pickSends, 2, 2, "METADATA.DB kobo|kindle" },
    { "json-get", Tool::jsonGet, 2, 2, "FILE.json KEY" },
    { "make-epub", Tool::makeEpub, 4, -1, "OUT.epub TITLE AUTHOR LANGUAGE PARAGRAPH..." },
    { "make-import-epubs", Tool::makeImportEpubs, 2, 2, "FOLDER COVER.jpg" },
    { "make-pdf", Tool::makePdf, 2, 4, "OUT.pdf TEXT [INFO.json [PACKET.xml]]" },
    { "make-library", Tool::makeLibrary, 2, 3, "FOLDER BOOKS [SEED]" },
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments().mid(1);
    // Run as Cousoir runs calibre-debug for a sending job.
    if (arguments.size() == 4 && arguments.at(0) == QLatin1String("-e")
        && QFileInfo(arguments.at(1)).fileName() == QLatin1String("send.py") && arguments.at(2) == QLatin1String("--"))
        return Tool::fakeSend(arguments.at(3));
    for (const Command &command : commands) {
        if (arguments.value(0) != QLatin1String(command.name))
            continue;
        const qsizetype count = arguments.size() - 1;
        if (count < command.minimum || (command.maximum >= 0 && count > command.maximum)) {
            Tool::err() << "usage: cousoir-test-tool " << command.name << ' ' << command.usage << Qt::endl;
            return 2;
        }
        return command.run(arguments.mid(1));
    }
    Tool::err() << "usage: cousoir-test-tool COMMAND ARGUMENT...\n\ncommands:\n";
    for (const Command &command : commands)
        Tool::err() << "  " << command.name << ' ' << command.usage << '\n';
    Tool::err().flush();
    return 2;
}
