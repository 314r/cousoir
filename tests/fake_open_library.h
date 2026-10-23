#pragma once

#include <QBuffer>
#include <QImage>
#include <QUrlQuery>

#include "fake_http_server.h"
#include "test_support.h"

inline QByteArray fakeJpeg(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG");
    return bytes;
}

// Open Library as the fixtures saved it: two searches, their works, and a
// cover of its own size for each cover id.
inline FakeHttpServer::Answer fakeOpenLibrary(const QUrl &url)
{
    const QUrlQuery query(url);
    const QString path = url.path();
    if (path == QLatin1String("/search.json")) {
        if (query.queryItemValue(QStringLiteral("isbn")) == QLatin1String("9782253140870"))
            return { 200, readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/openlibrary/search-isbn.json")) };
        if (query.hasQueryItem(QStringLiteral("isbn")))
            return { 200, R"({"numFound": 0, "docs": []})" };
        if (query.queryItemValue(QStringLiteral("title"), QUrl::FullyDecoded) == QLatin1String("Shadows Linger"))
            return { 200, readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/openlibrary/search-title.json")) };
        return { 200, R"({"numFound": 0, "docs": []})" };
    }
    if (path.startsWith(QLatin1String("/works/"))) {
        const QByteArray work = readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/openlibrary") + path);
        return work.isEmpty() ? FakeHttpServer::Answer{} : FakeHttpServer::Answer{ 200, work };
    }
    if (path.startsWith(QLatin1String("/b/id/"))) {
        const qint64 id = path.mid(6).section(u'-', 0, 0).toLongLong();
        // One cover missing, as Open Library says with default=false.
        if (id == 5542012)
            return {};
        return { 200, fakeJpeg(300 + int(id % 100), 450), "image/jpeg" };
    }
    return {};
}

