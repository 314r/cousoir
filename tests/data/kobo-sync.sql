-- The Voyage on a Kobo, as firmware 4.38 keeps a KEPUB put on it by hand,
-- written as a Kobo Libra 2 writes its own rows: where reading stopped in
-- the book's row, its files as rows of type 9 with "!!" between the book and
-- the file, its table of contents, the Kobo's user, and a highlight of the
-- Kobo's own. Beside it, a plain EPUB and a book bought from Kobo, whose
-- places Cousoir leaves alone. On top of tests/data/kobo.sql; the KEPUB,
-- the one Cousoir makes of tests/data/reader/voyage.epub, is put on the
-- Kobo by the tests.
CREATE TABLE user (UserID TEXT NOT NULL, UserKey TEXT NOT NULL, UserDisplayName TEXT, UserEmail TEXT,
    PRIMARY KEY (UserID));
INSERT INTO user VALUES ('0956f31c-0000-4000-8000-000000000001', 'key', 'Ann', NULL);
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, Attribution, ___UserID, Accessibility,
    IsDownloaded, ReadStatus, ___PercentRead, DateLastRead, FirstTimeReading, ChapterIDBookmarked,
    CurrentChapterProgress) VALUES
    ('file:///mnt/onboard/Ann Example/The Voyage.kepub.epub', '6', 'application/x-kobo-epub+zip', NULL, 'The Voyage',
     'Ann Example', 'kepub_user', -1, 'true', 1, 30, '2026-10-25T21:00:00Z', 'false', 'OEBPS/text/chapter2.xhtml#kobo.3.1', 0.0),
    ('file:///mnt/onboard/Ann Example/Another Voyage.epub', '6', 'application/epub+zip', NULL, 'Another Voyage',
     'Ann Example', 'adobe_user', -1, 'true', 1, 3, '2026-10-24T21:00:00Z', 'false', 'OPS/chapter-3.xhtml#point(/1/2/1/60/1/1:0)', 0.0),
    ('9c1d2e3f-4a5b-4c6d-8e7f-a0b1c2d3e4f5', '6', 'application/x-kobo-epub+zip', NULL, 'Bought Elsewhere', 'Store Author',
     'user', 1, 'true', 1, 5, '2026-10-23T21:00:00Z', 'false', 'OPS/chapter1.html#kobo.2.1', 0.0);
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, ___UserID, VolumeIndex, ___FileOffset, ___FileSize,
    ___PercentRead) VALUES
    ('/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/cover.xhtml', '9', 'application/xhtml+xml',
     'file:///mnt/onboard/Ann Example/The Voyage.kepub.epub', NULL, '', 0, 0, 1, 100),
    ('/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter1.xhtml', '9', 'application/xhtml+xml',
     'file:///mnt/onboard/Ann Example/The Voyage.kepub.epub', NULL, '', 1, 1, 40, 100),
    ('/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter2.xhtml', '9', 'application/xhtml+xml',
     'file:///mnt/onboard/Ann Example/The Voyage.kepub.epub', NULL, '', 2, 41, 40, 0),
    ('/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter3.xhtml', '9', 'application/xhtml+xml',
     'file:///mnt/onboard/Ann Example/The Voyage.kepub.epub', NULL, '', 3, 81, 19, 0);
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, ___UserID, VolumeIndex, ChapterIDBookmarked) VALUES
    ('/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter1.xhtml-1', '899', 'application/x-kobo-epub+zip',
     'file:///mnt/onboard/Ann Example/The Voyage.kepub.epub', 'The Harbour', '', 0,
     '/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter1.xhtml'),
    ('/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter2.xhtml-1', '899', 'application/x-kobo-epub+zip',
     'file:///mnt/onboard/Ann Example/The Voyage.kepub.epub', 'The Valley', '', 1,
     '/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter2.xhtml');
INSERT INTO Bookmark (BookmarkID, VolumeID, ContentID, StartContainerPath, StartContainerChildIndex, StartOffset,
    EndContainerPath, EndContainerChildIndex, EndOffset, Text, Annotation, DateCreated, ChapterProgress, Hidden,
    DateModified, UserID, Published, Type) VALUES
    ('c6fb154b-bc62-497f-be73-a332d2368434', 'file:///mnt/onboard/Ann Example/The Voyage.kepub.epub',
     '/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter1.xhtml', 'span#kobo\.3\.1', -99, 24,
     'span#kobo\.3\.1', -99, 53, 'Window winter candle lantern ', NULL, '2026-10-20T20:00:00.000', 0.05, 'false',
     '2026-10-20T20:00:00Z', '0956f31c-0000-4000-8000-000000000001', 'false', 'highlight');
