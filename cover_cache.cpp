#include "cover_cache.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QRunnable>
#include <QSaveFile>
#include <QThreadPool>

namespace {

struct Library
{
    QString thumbnailFolder;
    QHash<int, CoverCache::Cover> covers;
};

QMutex registryMutex;
QHash<QString, Library> registry;

class CoverResponse : public QQuickImageResponse, public QRunnable
{
public:
    CoverResponse(const QString &id, const QSize &requestedSize)
        : m_id(id)
        , m_requestedSize(requestedSize)
    {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override
    {
        return m_error;
    }

    void run() override
    {
        load();
        emit finished();
    }

private:
    void load()
    {
        // <key>/<book id>?v=<stamp>
        const QString path = m_id.section(u'?', 0, 0);
        const QString key = path.section(u'/', 0, 0);
        const int bookId = path.section(u'/', 1, 1).toInt();

        Library library;
        {
            QMutexLocker locker(&registryMutex);
            library = registry.value(key);
        }
        const CoverCache::Cover cover = library.covers.value(bookId);
        if (cover.file.isEmpty()) {
            m_error = QStringLiteral("Book %1 has no cover.").arg(bookId);
            return;
        }

        const bool scaled = m_requestedSize.isValid() && !m_requestedSize.isEmpty();
        const QString thumbnail = scaled && !library.thumbnailFolder.isEmpty()
            ? QDir(library.thumbnailFolder).filePath(QStringLiteral("%1-%2-%3x%4.jpg").arg(bookId).arg(cover.modifiedMs)
                                                         .arg(m_requestedSize.width()).arg(m_requestedSize.height()))
            : QString();
        if (!thumbnail.isEmpty() && m_image.load(thumbnail))
            return;

        QImageReader reader(cover.file);
        reader.setAutoTransform(true);
        if (scaled) {
            QSize size = reader.size();
            if (size.isValid()) {
                size.scale(m_requestedSize, Qt::KeepAspectRatio);
                // Decoding straight to the smaller size is much faster for JPEG.
                if (size.width() < reader.size().width())
                    reader.setScaledSize(size);
            }
        }
        if (!reader.read(&m_image)) {
            m_error = reader.errorString();
            return;
        }
        if (thumbnail.isEmpty())
            return;

        QDir().mkpath(library.thumbnailFolder);
        // Thumbnails of an older cover of this book go.
        const QString prefix = QStringLiteral("%1-").arg(bookId);
        const QString current = QStringLiteral("%1-%2-").arg(bookId).arg(cover.modifiedMs);
        const QStringList old = QDir(library.thumbnailFolder).entryList({ prefix + u'*' }, QDir::Files);
        for (const QString &file : old) {
            if (!file.startsWith(current))
                QFile::remove(QDir(library.thumbnailFolder).filePath(file));
        }
        QSaveFile out(thumbnail);
        if (out.open(QIODevice::WriteOnly) && m_image.convertToFormat(QImage::Format_RGB32).save(&out, "JPEG", 90))
            out.commit();
    }

    QString m_id;
    QSize m_requestedSize;
    QImage m_image;
    QString m_error;
};

} // namespace

void CoverCache::setCovers(const QString &key, const QString &thumbnailFolder, const QHash<int, Cover> &covers)
{
    QMutexLocker locker(&registryMutex);
    registry.insert(key, Library{ thumbnailFolder, covers });
}

QString CoverCache::url(const QString &key, int bookId, qint64 modifiedMs)
{
    return QStringLiteral("image://covers/%1/%2?v=%3").arg(key).arg(bookId).arg(modifiedMs);
}

QQuickImageResponse *CoverCache::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    auto *response = new CoverResponse(id, requestedSize);
    QThreadPool::globalInstance()->start(response);
    return response;
}
