-- Books a fake Kobo made from tests/data/kobo.sql has read, with the
-- highlights, notes and bookmarks made in them, as the Kobo writes them:
-- what Calibre's Kobo Bookmark reads, and its corners.
--
-- A KEPUB being read: its chapters are rows of type 899, "book!opf!file-N".
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, Attribution, ___UserID, Accessibility,
    IsDownloaded, ReadStatus, ___PercentRead, DateLastRead, TimeSpentReading)
VALUES ('file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub', '6', 'application/x-kobo-epub+zip', NULL,
    'L''Écume des jours', 'Boris Vian', '', -1, 'true', 1, 43, '2026-10-20T21:14:03Z', 5400);
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, ___UserID, VolumeIndex, ___NumPages)
VALUES ('file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch01.xhtml-1', '899', 'application/xhtml+xml',
        'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub', 'Un', '', 0, 10),
       ('file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch02.xhtml-1', '899', 'application/xhtml+xml',
        'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub', 'Deux', '', 1, 12),
       ('file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch03.xhtml-1', '899', 'application/xhtml+xml',
        'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub', 'Trois', '', 2, 8);
INSERT INTO Bookmark (BookmarkID, VolumeID, ContentID, StartContainerPath, StartContainerChildIndex, StartOffset,
    EndContainerPath, EndContainerChildIndex, EndOffset, Text, Annotation, DateCreated, ChapterProgress, Hidden)
VALUES
    -- A highlight, and one with a note, in the first chapter.
    ('e1', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub',
     'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch01.xhtml', 'span#kobo\.3\.1', 3, 10,
     'span#kobo\.3\.2', 3, 40, 'Colin achevait sa toilette.', NULL, '2026-10-18T20:01:00.000', 0.25, 'false'),
    ('e2', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub',
     'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch01.xhtml', 'span#kobo\.9\.1', 9, 2,
     'span#kobo\.9\.4', 9, 30, 'Chloé', 'Le nénuphar, déjà ?', '2026-10-18T20:15:00.000', 0.6, 'false'),
    -- Progress Calibre rounds to two decimals.
    ('e3', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub',
     'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch01.xhtml', 'span#kobo\.12\.1', 12, 1,
     'span#kobo\.12\.1', 12, 9, 'le pianocktail', '', '2026-10-18T20:20:00.000', 0.123456, 'false'),
    -- A corner turned down, and a highlight removed, hidden ever since.
    ('e4', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub',
     'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch02.xhtml', 'span#kobo\.1\.1', 0, 0,
     'span#kobo\.1\.1', 0, 0, NULL, NULL, '2026-10-19T07:00:00.000', 0.1, 'false'),
    ('e5', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub',
     'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch02.xhtml', 'span#kobo\.4\.1', 4, 1,
     'span#kobo\.4\.2', 4, 5, 'effacé', NULL, '2026-10-19T07:05:00.000', 0.3, 'true'),
    -- A note without text, and an empty highlight.
    ('e6', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub',
     'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch03.xhtml', 'span#kobo\.2\.1', 2, 1,
     'span#kobo\.2\.1', 2, 1, NULL, 'Seulement une note', '2026-10-20T21:00:00.000', 0.5, 'false'),
    ('e7', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub',
     'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch03.xhtml', 'span#kobo\.5\.1', 5, 1,
     'span#kobo\.5\.1', 5, 1, '', '', '2026-10-20T21:05:00.000', 0.9, 'false'),
    -- An id with no "!", whose chapter is none.
    ('e8', 'file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub', 'orphan', 'span#kobo\.1\.1', 1, 1,
     'span#kobo\.1\.1', 1, 4, 'perdu', NULL, '2026-10-20T21:10:00.000', 0.05, 'false');

-- An EPUB finished: its chapters are rows of type 9, which the bookmarks
-- name. Two chapters share an index, and the note of the second replaces
-- one of the first under Calibre's key; one bookmark names no chapter.
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, Attribution, ___UserID, Accessibility,
    IsDownloaded, ReadStatus, ___PercentRead, DateLastRead, TimeSpentReading)
VALUES ('file:///mnt/onboard/Pratchett, Terry/Mort.epub', '6', 'application/epub+zip', NULL, 'Mort',
    'Terry Pratchett', '', -1, 'true', 2, 87, '2026-09-01T08:00:00.000', 20000);
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, ___UserID, VolumeIndex, ___NumPages)
VALUES ('file:///mnt/onboard/Pratchett, Terry/Mort.epub#(0)OEBPS/c1.html', '9', 'application/xhtml+xml',
        'file:///mnt/onboard/Pratchett, Terry/Mort.epub', 'Chapter 1', '', 1, 20),
       ('file:///mnt/onboard/Pratchett, Terry/Mort.epub#(1)OEBPS/c2.html', '9', 'application/xhtml+xml',
        'file:///mnt/onboard/Pratchett, Terry/Mort.epub', 'Chapter 2', '', 2, 20),
       ('file:///mnt/onboard/Pratchett, Terry/Mort.epub#(2)OEBPS/c3.html', '9', 'application/xhtml+xml',
        'file:///mnt/onboard/Pratchett, Terry/Mort.epub', 'Chapter 3', '', 1, 20);
INSERT INTO Bookmark (BookmarkID, VolumeID, ContentID, StartContainerPath, StartContainerChildIndex, StartOffset,
    EndContainerPath, EndContainerChildIndex, EndOffset, Text, Annotation, DateCreated, ChapterProgress, Hidden)
VALUES
    ('m1', 'file:///mnt/onboard/Pratchett, Terry/Mort.epub',
     'file:///mnt/onboard/Pratchett, Terry/Mort.epub#(0)OEBPS/c1.html', '/1/4/2', 2, 5, '/1/4/2', 2, 30,
     'THERE IS NO JUSTICE.', NULL, '2026-08-20T10:00:00.000', 0.5, 'false'),
    ('m2', 'file:///mnt/onboard/Pratchett, Terry/Mort.epub',
     'file:///mnt/onboard/Pratchett, Terry/Mort.epub#(0)OEBPS/c1.html', '/1/4/1', 1, 5, '/1/4/1', 1, 30,
     'THERE IS JUST ME.', 'Death, again', '2026-08-20T10:05:00.000', 0.1, 'false'),
    ('m3', 'file:///mnt/onboard/Pratchett, Terry/Mort.epub',
     'file:///mnt/onboard/Pratchett, Terry/Mort.epub#(1)OEBPS/c2.html', '/1/6/3', 3, 1, '/1/6/3', 3, 12,
     'Mort', NULL, '2026-08-21T10:00:00.000', 1.2, 'false'),
    ('m4', 'file:///mnt/onboard/Pratchett, Terry/Mort.epub',
     'file:///mnt/onboard/Pratchett, Terry/Mort.epub#(2)OEBPS/c3.html', '/1/2/1', 1, 1, '/1/2/1', 1, 8,
     'Binky', NULL, '2026-08-22T10:00:00.000', 0.7, 'false'),
    ('m5', 'file:///mnt/onboard/Pratchett, Terry/Mort.epub',
     'file:///mnt/onboard/Pratchett, Terry/Mort.epub#(9)gone.html', '/1/2/1', 1, 1, '/1/2/1', 1, 8,
     'Ysabell', NULL, '2026-08-23T10:00:00.000', 0.2, 'false');

-- A book bought from Kobo, not begun, with a bookmark all the same.
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, Attribution, ___UserID, Accessibility,
    IsDownloaded, ReadStatus, ___PercentRead)
VALUES ('5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d', '6', 'application/x-kobo-epub+zip', NULL, 'Bought Book',
    'Store Author', 'user', 1, 'true', 0, 0);
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, ___UserID, VolumeIndex, ___NumPages)
VALUES ('5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d!OPS!chapter1.html-1', '899', 'application/xhtml+xml',
        '5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d', 'Opening', 'user', 0, 5);
INSERT INTO Bookmark (BookmarkID, VolumeID, ContentID, StartContainerPath, StartContainerChildIndex, StartOffset,
    EndContainerPath, EndContainerChildIndex, EndOffset, Text, Annotation, DateCreated, ChapterProgress, Hidden)
VALUES ('p1', '5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d', '5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d!OPS!chapter1.html',
     'span#kobo\.1\.1', 1, 2, 'span#kobo\.1\.1', 1, 9, 'Bought words', NULL, '2026-10-01T09:00:00.000', 0.4, 'false'),
    -- 3.125 % exactly, which Python's round() takes to the even 3.12.
    ('p2', '5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d', '5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d!OPS!chapter1.html',
     'span#kobo\.2\.1', 2, 1, 'span#kobo\.2\.1', 2, 6, 'Early words', NULL, '2026-10-01T08:00:00.000', 0.03125, 'false');

-- Being read, but the Kobo has no percent for it.
INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, Attribution, ___UserID, Accessibility,
    IsDownloaded, ReadStatus, ___PercentRead, DateLastRead)
VALUES ('file:///mnt/onboard/Notes.pdf', '6', 'application/pdf', NULL, 'Notes', NULL, '', -1, 'true', 1, NULL,
    '2026-10-21T12:00:00Z');
