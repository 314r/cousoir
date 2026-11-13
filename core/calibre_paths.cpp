// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/backend.py: construct_path_name(), construct_file_name()
//   and WINDOWS_RESERVED_NAMES
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "calibre_paths.h"

#include <QStringList>

#include "calibre_text.h"

namespace {

QString leftStripped(const QString &text)
{
    qsizetype start = 0;
    while (start < text.size() && CalibreText::isSpace(text.at(start)))
        ++start;
    return text.mid(start);
}

QString rightStripped(const QString &text)
{
    qsizetype end = text.size();
    while (end > 0 && CalibreText::isSpace(text.at(end - 1)))
        --end;
    return text.left(end);
}

bool isWindowsReservedName(const QString &name)
{
    static const QStringList reserved = QStringLiteral(
        "CON PRN AUX NUL COM1 COM2 COM3 COM4 COM5 COM6 COM7 COM8 COM9 LPT1 LPT2 LPT3 LPT4 LPT5 LPT6 LPT7 LPT8 LPT9")
                                            .split(u' ');
    return reserved.contains(name.toUpper());
}

// The title part shared by both names: ASCII, cut to `limit`, "Unknown" when
// nothing is left.
QString titlePart(const QString &title, int limit)
{
    const QString part = rightStripped(CalibreText::asciiFilename(leftStripped(title)).left(limit));
    return part.isEmpty() ? QStringLiteral("Unknown").left(limit) : part;
}

} // namespace

namespace CalibrePaths {

QString constructPathName(int bookId, const QString &title, const QString &author, const QString &unknown)
{
    const QString id = QStringLiteral(" (%1)").arg(bookId);
    const int limit = PathLimit - int(id.size()) / 2 - 2;
    QString folder = CalibreText::asciiFilename(author).left(limit);
    while (!folder.isEmpty() && (folder.back() == u' ' || folder.back() == u'.'))
        folder.chop(1);
    if (folder.isEmpty())
        folder = CalibreText::asciiFilename(unknown);
    if (isWindowsReservedName(folder))
        folder += u'w';
    return folder + u'/' + titlePart(title, limit) + id;
}

QString constructFileName(int, const QString &title, const QString &author, int extensionLength,
                          const QString &unknown)
{
    // 14 is the length of ORIGINAL_EPUB.
    const int limit = (PathLimit - std::max(extensionLength, 14) - 2) / 2;
    QString name = titlePart(title, limit) + QStringLiteral(" - ") + CalibreText::asciiFilename(author).left(limit);
    while (name.endsWith(u'.'))
        name.chop(1);
    return name.isEmpty() ? CalibreText::asciiFilename(unknown) : name;
}

} // namespace CalibrePaths
