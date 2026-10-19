-- The tables of a Kobo's .kobo/KoboReader.sqlite that Calibre's KoboTouch
-- driver reads or writes, for a fake Kobo in the tests. The columns of
-- content are those of firmware 4.38, which Calibre selects or updates.
CREATE TABLE DbVersion (version INTEGER);
INSERT INTO DbVersion VALUES (170);
CREATE TABLE content (
    ContentID TEXT NOT NULL, ContentType TEXT NOT NULL, MimeType TEXT NOT NULL, BookID TEXT, BookTitle TEXT,
    ImageId TEXT, Title TEXT COLLATE NOCASE, Attribution TEXT COLLATE NOCASE, Description TEXT, DateCreated TEXT,
    ShortCoverKey TEXT, adobe_location TEXT, Publisher TEXT, IsEncrypted BOOL, DateLastRead TEXT,
    FirstTimeReading BOOL, ChapterIDBookmarked TEXT, ParagraphBookmarked INTEGER, BookmarkWordOffset INTEGER,
    NumShortcovers INTEGER, VolumeIndex INTEGER, ___NumPages INTEGER, ReadStatus INTEGER, ___SyncTime TEXT,
    ___UserID TEXT NOT NULL, PublicationId TEXT, ___FileOffset INTEGER, ___FileSize INTEGER, ___PercentRead INTEGER,
    ___ExpirationStatus INTEGER, FavouritesIndex NUMERIC NOT NULL DEFAULT -1, Accessibility INTEGER DEFAULT 1,
    ContentURL TEXT, Language TEXT, BookshelfTags TEXT, IsDownloaded BIT NOT NULL DEFAULT 1,
    FeedbackType INTEGER DEFAULT 0, AverageRating INTEGER DEFAULT 0, Depth INTEGER, PageProgressDirection TEXT,
    InWishlist TEXT NOT NULL DEFAULT 'FALSE', ISBN TEXT, WishlistedDate TEXT NOT NULL DEFAULT '0000-00-00T00:00:00.000',
    FeedbackTypeSynced INTEGER NOT NULL DEFAULT 0, IsSocialEnabled BOOL NOT NULL DEFAULT 'true', EpubType INTEGER DEFAULT -1,
    Monetization INTEGER DEFAULT 2, ExternalId TEXT, Series TEXT, SeriesNumber TEXT, Subtitle TEXT,
    WordCount INTEGER DEFAULT -1, Fallback TEXT, RestOfBookEstimate INTEGER, CurrentChapterEstimate INTEGER,
    CurrentChapterProgress FLOAT, PocketStatus INTEGER DEFAULT 0, UnsyncedPocketChanges TEXT, ImageUrl TEXT,
    DateAdded TEXT, WorkId TEXT, Properties TEXT, RenditionSpread TEXT, RatingCount INTEGER DEFAULT 0,
    ReviewsSyncDate TEXT, MediaOverlay TEXT, MediaOverlayType TEXT, RedirectPreviewUrl BOOL, PreviewFileSize INTEGER,
    EntitlementId TEXT, CrossRevisionId TEXT, DownloadUrl BOOL, ReadStateSynced BOOL DEFAULT false,
    TimesStartedReading INTEGER, TimeSpentReading INTEGER, LastTimeStartedReading TEXT, LastTimeFinishedReading TEXT,
    ApplicableSubscriptions TEXT, ExternalIds TEXT, PurchaseRevisionId TEXT, SeriesID TEXT, SeriesNumberFloat REAL,
    AdobeLoanExpiration TEXT, HideFromHomePage BOOL, IsInternetArchive BOOL, titleKana TEXT, subtitleKana TEXT,
    seriesKana TEXT, attributionKana TEXT, publisherKana TEXT, IsPurchaseable BOOL, IsSupported BOOL,
    AnnotationsSyncToken TEXT, DateModified TEXT, StorePages INTEGER, StoreWordCount INTEGER,
    StoreTimeToReadLowerEstimate INTEGER, StoreTimeToReadUpperEstimate INTEGER, Duration INTEGER,
    IsAbridged BOOL, SyncConflictType INTEGER, PRIMARY KEY (ContentID));
CREATE TABLE Shelf (CreationDate TEXT, Id TEXT, InternalName TEXT, LastModified TEXT, Name TEXT, Type TEXT,
    _IsDeleted BOOL, _IsVisible BOOL, _IsSynced BOOL, _SyncTime TEXT, LastAccessed TEXT, IsPaused BOOL,
    PRIMARY KEY (Id));
CREATE TABLE ShelfContent (ShelfName TEXT, ContentId TEXT, DateModified TEXT, _IsDeleted BOOL, _IsSynced BOOL,
    PRIMARY KEY (ShelfName, ContentId));
CREATE TABLE Bookmark (BookmarkID TEXT NOT NULL, VolumeID TEXT NOT NULL, ContentID TEXT NOT NULL,
    StartContainerPath TEXT, EndContainerPath TEXT, Text TEXT, Annotation TEXT, DateCreated TEXT,
    PRIMARY KEY (BookmarkID));
CREATE TABLE volume_shortcovers (volumeId TEXT NOT NULL, shortcoverId TEXT NOT NULL, VolumeIndex INTEGER,
    PRIMARY KEY (volumeId, shortcoverId));
CREATE TABLE content_keys (volumeId TEXT NOT NULL, elementId TEXT NOT NULL, elementKey TEXT,
    PRIMARY KEY (volumeId, elementId));
CREATE TABLE content_settings (ContentID TEXT, ContentType TEXT, DateModified TEXT, ReadingFontFamily TEXT,
    PRIMARY KEY (ContentID, ContentType));
CREATE TABLE ratings (ContentID TEXT NOT NULL, Rating INTEGER, Review TEXT, DateModified TEXT,
    PRIMARY KEY (ContentID));
CREATE TABLE Activity (Id TEXT NOT NULL, Enabled BOOL, Type TEXT NOT NULL, Action INTEGER, Date TEXT, Data BLOB,
    PRIMARY KEY (Id, Type));
