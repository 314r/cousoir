#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <memory>

// An EPUB opened for reading: its package's spine and table of contents,
// and its files read as they are asked for, by their names in the archive,
// as Calibre's viewer names them ("OEBPS/Text/chapter1.xhtml").
class EpubBook
{
public:
    struct SpineItem
    {
        QString name;
        bool linear = true;
        // The file's size, for how far into the book a position is.
        qint64 size = 0;
        // A page laid out once, at its own size (EPUB 3's pre-paginated
        // layout), as comics and picture books are, and the side of a
        // spread it goes on: "left", "right", or "center" for one shown
        // alone.
        bool fixed = false;
        QString side;
    };

    struct TocEntry
    {
        QString title;
        // The file, and the fragment within it; empty for a heading alone.
        QString name;
        QString fragment;
        int depth = 0;
    };

    // The book at `path`, or nullptr with `error`.
    static std::unique_ptr<EpubBook> open(const QString &path, QString *error);
    ~EpubBook();

    QString title() const { return m_title; }
    QString language() const { return m_language; }
    // "rtl" for a book read right to left, as its spine says.
    QString direction() const { return m_direction; }
    const QList<SpineItem> &spine() const { return m_spine; }
    const QList<TocEntry> &toc() const { return m_toc; }
    // The spine item a name is, -1 for none.
    int spineIndex(const QString &name) const;

    bool contains(const QString &name) const;
    QByteArray read(const QString &name) const;
    // The media type the package gives the file, else one by its extension.
    QString mediaType(const QString &name) const;

    // A path of `href`, relative to the file `base`, as an archive name:
    // "../Images/a.png" from "OEBPS/Text/c.xhtml" is "OEBPS/Images/a.png".
    static QString resolve(const QString &base, const QString &href);

private:
    EpubBook() = default;
    bool load(QString *error);
    void readNav(const QString &name);
    void readNcx(const QString &name);

    void *m_zip = nullptr;
    QString m_path;
    QHash<QString, qint64> m_sizes;
    QHash<QString, QString> m_types;
    QString m_title;
    QString m_language;
    QString m_direction;
    QList<SpineItem> m_spine;
    QList<TocEntry> m_toc;
    mutable QMutex m_lock;
};
