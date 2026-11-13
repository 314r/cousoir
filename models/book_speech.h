#pragma once

#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class BookSentences;
class EpubBook;
class QTextToSpeech;

// A book read aloud by the system's voices, through Qt TextToSpeech: a
// sentence after the other from a place, through the spine items to the
// book's end. The sentence read, and the word when the engine tells it, by
// their CFIs, for the page to mark and follow.
class BookSpeech : public QObject
{
    Q_OBJECT
    // The engines installed: "speechd", "flite"…; the engine picked, empty
    // for Qt's default.
    Q_PROPERTY(QStringList engines READ engines CONSTANT)
    Q_PROPERTY(QString engine READ engine WRITE setEngine NOTIFY engineChanged)
    // The voices of the engine for the book's language, or all of them when
    // it has none: { name, label }; after loadVoices().
    Q_PROPERTY(QVariantList voices READ voices NOTIFY voicesChanged)
    // The voice picked, by its name; empty for the engine's own.
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY voiceChanged)
    // From -1, slowest, to 1, fastest; 0 the voice's own.
    Q_PROPERTY(double rate READ rate WRITE setRate NOTIFY rateChanged)
    // "stopped", "speaking" or "paused".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // Why the engine cannot speak, once it failed, until stopped.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // The sentence read: { spineIndex, start, end, text }, CFIs within the
    // spine item; empty when nothing is.
    Q_PROPERTY(QVariantMap sentence READ sentence NOTIFY sentenceChanged)
    // The CFI of the word read in it, when the engine tells it.
    Q_PROPERTY(QString word READ word NOTIFY wordChanged)

public:
    explicit BookSpeech(QObject *parent = nullptr);
    ~BookSpeech() override;

    // The book read, and its language, for the voices; null stops reading.
    void setBook(std::shared_ptr<const EpubBook> book, const QString &language);

    QStringList engines() const;
    QString engine() const;
    void setEngine(const QString &engine);
    QVariantList voices() const;
    QString voice() const;
    void setVoice(const QString &voice);
    double rate() const;
    void setRate(double rate);
    QString state() const;
    QString error() const;
    QVariantMap sentence() const;
    QString word() const;

    // Starts the engine, if not yet, for its voices.
    Q_INVOKABLE void loadVoices();
    // Reads from the sentence a CFI of the spine item is in.
    Q_INVOKABLE void start(int spineIndex, const QString &cfi);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    // Reads on from `count` sentences after the one read, or before it.
    Q_INVOKABLE void skip(int count);

signals:
    void engineChanged();
    void voicesChanged();
    void voiceChanged();
    void rateChanged();
    void stateChanged();
    void sentenceChanged();
    void wordChanged();

private:
    // A sentence of the book: its spine item's sentences, and which.
    struct Place
    {
        std::shared_ptr<const BookSentences> sentences;
        int spineIndex = -1;
        int index = -1;
    };

    bool ensureEngine();
    void applyVoice();
    // The sentences of a spine item, read once while needed.
    std::shared_ptr<const BookSentences> sentencesOf(int spineIndex);
    // The place `count` sentences from `from`, across spine items; an
    // invalid one past the book's ends.
    Place moved(Place from, int count);
    // Says the sentence at `place`, the one read from now on.
    void speak(const Place &place);
    // The engine stopped, nothing read.
    void silence();
    void setMode(const QString &mode);

    std::unique_ptr<QTextToSpeech> m_tts;
    QString m_engine;
    QString m_voice;
    double m_rate = 0;
    std::shared_ptr<const EpubBook> m_book;
    QString m_language;
    QHash<int, std::shared_ptr<const BookSentences>> m_cache;
    // The sentence read. Qt's queue of texts is left alone: its ids repeat,
    // and it says texts given while it moves to the next out of turn.
    Place m_current;
    // Said, the engine not yet speaking it: a Ready meanwhile is the end of
    // the one stopped, not of this one.
    bool m_starting = false;
    QString m_mode = QStringLiteral("stopped");
    // While the engine is stopped here.
    bool m_stopping = false;
    QString m_error;
    QString m_word;
};
