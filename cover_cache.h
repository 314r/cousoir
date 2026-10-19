#pragma once

#include <QHash>
#include <QQuickAsyncImageProvider>
#include <QString>

// Book covers for QML, as image://covers/<library key>/<book id>?v=<stamp>.
// A requested size scales the cover on a pool thread and keeps the result in
// the library's cache folder, so the cover grid opens without decoding every
// cover again. Covers are only ever read from the library.
class CoverCache : public QQuickAsyncImageProvider
{
public:
    struct Cover
    {
        QString file;
        qint64 modifiedMs = 0;
    };

    // Replaces the covers known for the library `key`, with the folder their
    // thumbnails go to.
    static void setCovers(const QString &key, const QString &thumbnailFolder, const QHash<int, Cover> &covers);
    // The image source for a book's cover, empty when it has none.
    static QString url(const QString &key, int bookId, qint64 modifiedMs);

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
};
