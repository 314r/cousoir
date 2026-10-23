-- A Kobo database as kbextract2 tests read it: books, chapters split over
-- several files, hidden rows, a corner turned down, a note alone. Copied
-- from kbextract2's internal/kobo/testdata/fixture.sql,
-- https://github.com/314r/kbextract2, Copyright (c) 2026 314r, MIT license.
CREATE TABLE content (
    ContentID TEXT, BookID TEXT, Title TEXT, Attribution TEXT,
    ContentType INTEGER, VolumeIndex INTEGER, ChapterIDBookmarked TEXT);
CREATE TABLE Bookmark (
    BookmarkID TEXT, VolumeID TEXT, ContentID TEXT, Text TEXT, Annotation TEXT,
    Hidden TEXT, StartContainerChildIndex INTEGER, StartOffset INTEGER, ChapterProgress REAL);
INSERT INTO content VALUES
    ('book-alpha', NULL, 'Alpha', 'Author A', 6, 0, NULL),
    ('book-beta', NULL, 'beta', 'Author B', 6, 0, NULL),
    ('book-empty', NULL, 'Empty', 'Author C', 6, 0, NULL),
    ('book-split', NULL, 'Le labyrinthe des égarés', 'Amin Maalouf', 6, -1, NULL),
    ('chapter-1', 'book-alpha', 'Chapter' || char(10) || 'One', NULL, 9, 1, NULL),
    ('chapter-2', 'book-alpha', 'Chapter Two', NULL, 9, 2, NULL),
    ('book-beta!opf!chapter-2-1', 'book-beta', 'Beta Chapter', NULL, 899, 2,
        'book-beta!opf!chapter-2'),
    ('book-split!OPS!p1.xhtml', 'book-split', 'p1.xhtml', NULL, 9, 4, NULL),
    ('book-split!OPS!p1chap8.xhtml', 'book-split', 'p1chap8.xhtml', NULL, 9, 12, NULL),
    ('book-split!OPS!p1chap10.xhtml', 'book-split', 'p1chap10.xhtml', NULL, 9, 14, NULL),
    ('book-split!OPS!p3.xhtml', 'book-split', 'p3.xhtml', NULL, 9, 26, NULL),
    ('book-split!OPS!p3chap11.xhtml', 'book-split', 'p3chap11.xhtml', NULL, 9, 37, NULL),
    ('book-split!OPS!p5.xhtml', 'book-split', 'p5.xhtml', NULL, 9, 47, NULL),
    ('book-split!OPS!p5chap3.xhtml', 'book-split', 'p5chap3.xhtml', NULL, 9, 50, NULL),
    ('book-split!OPS!p5chap4.xhtml', 'book-split', 'p5chap4.xhtml', NULL, 9, 51, NULL),
    ('book-split!OPS!p1.xhtml-1', 'book-split', 'I.' || char(8194) || 'Les étincelles japonaises', NULL, 899, 4,
        'book-split!OPS!p1.xhtml'),
    ('book-split!OPS!p3.xhtml-1', 'book-split', 'III.' || char(8194) || 'Une si longue marche', NULL, 899, 6,
        'book-split!OPS!p3.xhtml'),
    ('book-split!OPS!p5.xhtml-1', 'book-split', 'Épilogue.' || char(8194) || 'Un monde à reconstruire', NULL, 899, 8,
        'book-split!OPS!p5.xhtml');
INSERT INTO Bookmark VALUES
    ('later-highlight', 'book-alpha', 'chapter-2', 'Later text', NULL, 'false', 1, 4, 0.2),
    ('special-highlight', 'book-alpha', 'chapter-1',
        'Stars *stay* & <tag>' || char(10) || 'second line' || char(10) || char(10)
        || 'Another paragraph' || char(10) || 'continues here', NULL, 'false', 1, 5, 0.1),
    ('note', 'book-alpha', 'chapter-1', 'Selected note text',
        'Written note' || char(10) || 'second note line', 'false', 1, 8, 0.2),
    ('hidden', 'book-alpha', 'chapter-1', 'Hidden text', NULL, 'true', 1, 12, 0.3),
    ('dog-ear', 'book-alpha', 'chapter-1', 'Chapter bookmark', NULL, 'false', 0, 0, 0.4),
    ('annotation-only', 'book-beta', 'book-beta!opf!chapter-2', NULL, 'Margin note', '0', 2, 3, 0.1),
    ('split-p1-later', 'book-split', 'book-split!OPS!p1chap10.xhtml',
        'Second part-one highlight', NULL, 'false', 1, 2, 0.1),
    ('split-p1-earlier', 'book-split', 'book-split!OPS!p1chap8.xhtml',
        'First part-one highlight', NULL, 'false', 1, 2, 0.1),
    ('split-p3', 'book-split', 'book-split!OPS!p3chap11.xhtml',
        'Part-three highlight', NULL, 'false', 1, 2, 0.1),
    ('split-p5-first', 'book-split', 'book-split!OPS!p5chap3.xhtml',
        'First epilogue highlight', NULL, 'false', 1, 2, 0.1),
    ('split-p5-second', 'book-split', 'book-split!OPS!p5chap4.xhtml',
        'Second epilogue highlight', NULL, 'false', 1, 2, 0.1),
    ('fallback-title', 'missing-volume', 'chapter-3', 'Orphaned highlight', NULL, 'false', 3, 2, 0.1),
    ('blank', 'book-empty', 'chapter-4', '   ', '   ', 'false', 4, 2, 0.1);
