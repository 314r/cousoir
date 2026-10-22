#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class BookLibrary;
class EpubBook;

// A library book read in Cousoir: its EPUB served to Qt WebEngine as
// cousoir-book://book-<id>/<name in the archive>, its spine and table of
// contents, and where reading stopped. Positions are CFIs as Calibre's viewer
// writes them, "epubcfi(/6/2/4/2/1:120)": the spine item's step, then the
// path in its document from the document node (see assets/reader/reader.js).
class BookReader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    Q_PROPERTY(int bookId READ bookId NOTIFY bookChanged)
    Q_PROPERTY(QString title READ title NOTIFY bookChanged)
    Q_PROPERTY(QString format READ format NOTIFY bookChanged)
    // { url, name, size } by spine item.
    Q_PROPERTY(QVariantList spine READ spine NOTIFY bookChanged)
    // { title, spineIndex, fragment, depth }.
    Q_PROPERTY(QVariantList toc READ toc NOTIFY bookChanged)
    // Where to open: { spineIndex, cfi, device }, the newest position kept,
    // Cousoir's own or one in the library's, of Calibre's viewer for
    // instance.
    Q_PROPERTY(QVariantMap startPosition READ startPosition NOTIFY bookChanged)
    // A PDF's file, which Qt Pdf shows.
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY bookChanged)
    // The script that pages a document and finds positions in it.
    Q_PROPERTY(QString script READ script CONSTANT)
    // The highlights of Calibre's viewer and Cousoir's in the book: { uuid,
    // spineIndex, start, end, text, notes, which, timestamp }, in the book's
    // order.
    Q_PROPERTY(QVariantList highlights READ highlights NOTIFY highlightsChanged)

public:
    explicit BookReader(QObject *parent = nullptr);
    ~BookReader() override;

    // Before the application is made: the cousoir-book scheme and Qt
    // WebEngine; then ready() tells it was done.
    static void initialize();
    static bool ready();

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    int bookId() const;
    QString title() const;
    QString format() const;
    QVariantList spine() const;
    QVariantList toc() const;
    QVariantMap startPosition() const;
    QString script() const;
    QVariantList highlights() const;
    QUrl fileUrl() const;

    // The EPUB format a book is read in, empty for none.
    static QString readableFormat(const QVariantList &formats);

    // Opens the book's EPUB. Returns why it cannot, empty once open.
    Q_INVOKABLE QString open(int bookId);
    // Opens the book's PDF; startPosition then has its { page }.
    Q_INVOKABLE QString openPdf(int bookId);
    // Keeps the page of a PDF being read.
    Q_INVOKABLE void savePage(int page, int pages);
    Q_INVOKABLE void close();
    // The spine item of an address of the book, -1 for another.
    Q_INVOKABLE int spineIndexOf(const QUrl &url) const;
    // How far into the book a position in a spine item is, from 0 to 1.
    Q_INVOKABLE double bookFraction(int spineIndex, double fileFraction) const;
    // Keeps where reading is: a CFI in the spine item, and how far into it.
    Q_INVOKABLE void savePosition(int spineIndex, const QString &cfi, double fileFraction);
    // A highlight of the text from `start` to `end`, CFIs within the spine
    // item, in Calibre's colour `which` ("yellow", "green", "blue", "red",
    // "purple"), kept where Calibre's viewer keeps its own. Returns its id.
    Q_INVOKABLE QString addHighlight(int spineIndex, const QString &start, const QString &end, const QString &text,
                                     const QString &which = QStringLiteral("yellow"));
    // As Calibre's viewer removes one: kept, marked removed, for the
    // programs that have it to forget it too.
    Q_INVOKABLE void removeHighlight(const QString &uuid);

signals:
    void libraryChanged();
    void bookChanged();
    void highlightsChanged();

private:
    QString positionsFile() const;

    QPointer<BookLibrary> m_library;
    std::shared_ptr<EpubBook> m_book;
    int m_bookId = 0;
    QString m_uuid;
    QString m_format;
    QString m_token;
    QString m_pdfPath;
    QString m_pdfTitle;
    QVariantMap m_start;
    QString m_lastCfi;
    double m_lastFraction = 0;
    // By uuid, as Calibre's viewer keeps them.
    QMap<QString, QJsonObject> m_highlights;
};
