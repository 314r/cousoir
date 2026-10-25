// SPDX-License-Identifier: GPL-3.0-only

#include "book_speech.h"

#include <QLocale>
#include <QTextToSpeech>
#include <QVoice>

#include <optional>

#include "book_search.h"
#include "epub_book.h"

BookSpeech::BookSpeech(QObject *parent) : QObject(parent) {}

BookSpeech::~BookSpeech()
{
    if (m_tts)
        m_tts->stop(QTextToSpeech::BoundaryHint::Immediate);
}

void BookSpeech::setBook(std::shared_ptr<const EpubBook> book, const QString &language)
{
    stop();
    m_book = std::move(book);
    m_cache.clear();
    if (m_language != language) {
        m_language = language;
        applyVoice();
        emit voicesChanged();
    }
}

QStringList BookSpeech::engines() const
{
    QStringList engines = QTextToSpeech::availableEngines();
    // Qt's own, for the tests, says nothing.
    if (m_engine != QLatin1String("mock"))
        engines.removeAll(QStringLiteral("mock"));
    return engines;
}

QString BookSpeech::engine() const
{
    return m_engine;
}

void BookSpeech::setEngine(const QString &engine)
{
    if (m_engine == engine)
        return;
    stop();
    m_engine = engine;
    const bool had = m_tts != nullptr;
    m_tts.reset();
    m_error.clear();
    if (had)
        ensureEngine();
    emit engineChanged();
    emit voicesChanged();
}

QVariantList BookSpeech::voices() const
{
    QVariantList list;
    if (!m_tts)
        return list;
    for (const QVoice &voice : m_tts->availableVoices()) {
        const QString language = QLocale::languageToString(voice.language());
        list.append(QVariantMap{ { QStringLiteral("name"), voice.name() },
                                 { QStringLiteral("label"), language.isEmpty() || voice.name().contains(language)
                                                                ? voice.name()
                                                                : QStringLiteral("%1 (%2)").arg(voice.name(), language) } });
    }
    return list;
}

QString BookSpeech::voice() const
{
    return m_voice;
}

void BookSpeech::setVoice(const QString &voice)
{
    if (m_voice == voice)
        return;
    m_voice = voice;
    applyVoice();
    emit voiceChanged();
}

double BookSpeech::rate() const
{
    return m_rate;
}

void BookSpeech::setRate(double rate)
{
    rate = std::clamp(rate, -1.0, 1.0);
    if (qFuzzyCompare(m_rate + 2, rate + 2))
        return;
    m_rate = rate;
    if (m_tts)
        m_tts->setRate(m_rate);
    emit rateChanged();
}

QString BookSpeech::state() const
{
    return m_mode;
}

QString BookSpeech::error() const
{
    return m_error;
}

QVariantMap BookSpeech::sentence() const
{
    if (!m_current.sentences || m_current.index < 0)
        return {};
    const BookSentences::Sentence &sentence = m_current.sentences->sentences().at(m_current.index);
    return { { QStringLiteral("spineIndex"), m_current.spineIndex },
             { QStringLiteral("start"), sentence.start },
             { QStringLiteral("end"), sentence.end },
             { QStringLiteral("text"), sentence.text } };
}

QString BookSpeech::word() const
{
    return m_word;
}

void BookSpeech::loadVoices()
{
    if (!m_tts && ensureEngine())
        emit voicesChanged();
}

bool BookSpeech::ensureEngine()
{
    if (m_tts)
        return !m_tts->engine().isEmpty() && m_tts->state() != QTextToSpeech::Error;
    // The engine picked; else Qt's first, and when it cannot speak (its
    // plug-in's library or its service missing), the others.
    QStringList candidates{ m_engine };
    if (m_engine.isEmpty())
        candidates += engines();
    for (const QString &candidate : std::as_const(candidates)) {
        m_tts = candidate.isEmpty() ? std::make_unique<QTextToSpeech>() : std::make_unique<QTextToSpeech>(candidate);
        if (!m_tts->engine().isEmpty() && m_tts->state() != QTextToSpeech::Error)
            break;
    }
    if (m_tts->engine().isEmpty() || m_tts->state() == QTextToSpeech::Error) {
        m_error = tr("no speech engine works here; speech-dispatcher with a voice, or Flite, would");
        emit stateChanged();
        return false;
    }
    connect(m_tts.get(), &QTextToSpeech::sayingWord, this, [this](const QString &, qsizetype, qsizetype start, qsizetype) {
        if (!m_current.sentences || m_starting)
            return;
        m_word = m_current.sentences->cfiAt(m_current.sentences->sentences().at(m_current.index).at + start);
        emit wordChanged();
    });
    connect(m_tts.get(), &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State state) {
        if (state == QTextToSpeech::Error) {
            m_error = m_tts->errorString();
            silence();
            setMode(QStringLiteral("stopped"));
            emit stateChanged();
        } else if (state == QTextToSpeech::Speaking) {
            m_starting = false;
        } else if (state == QTextToSpeech::Ready && !m_stopping && !m_starting && m_mode == QLatin1String("speaking")) {
            // The sentence said: the next, to the book's end.
            const Place next = moved(m_current, 1);
            if (next.sentences) {
                speak(next);
            } else {
                silence();
                setMode(QStringLiteral("stopped"));
            }
        }
    });
    m_tts->setRate(m_rate);
    applyVoice();
    return true;
}

void BookSpeech::applyVoice()
{
    if (!m_tts)
        return;
    // The book's language, when the engine speaks it: its country's, else
    // another's.
    const QLocale wanted(m_language);
    if (!m_language.isEmpty() && wanted.language() != QLocale::C) {
        std::optional<QLocale> found;
        for (const QLocale &locale : m_tts->availableLocales()) {
            if (locale.language() == wanted.language() && (!found || locale.territory() == wanted.territory()))
                found = locale;
        }
        if (found)
            m_tts->setLocale(*found);
    }
    for (const QVoice &voice : m_tts->availableVoices()) {
        if (voice.name() == m_voice) {
            m_tts->setVoice(voice);
            break;
        }
    }
}

std::shared_ptr<const BookSentences> BookSpeech::sentencesOf(int spineIndex)
{
    if (!m_book || spineIndex < 0 || spineIndex >= m_book->spine().size())
        return {};
    auto found = m_cache.constFind(spineIndex);
    if (found != m_cache.cend())
        return *found;
    // Those read and to read only.
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.key() < spineIndex - 1 || it.key() > spineIndex + 1)
            it = m_cache.erase(it);
        else
            ++it;
    }
    auto sentences = std::make_shared<const BookSentences>(*m_book, spineIndex);
    m_cache.insert(spineIndex, sentences);
    return sentences;
}

BookSpeech::Place BookSpeech::moved(Place from, int count)
{
    Place place = from;
    place.index += count;
    // Across spine items, those without a sentence passed.
    while (place.sentences && place.index >= place.sentences->sentences().size()) {
        const int spare = place.index - int(place.sentences->sentences().size());
        place.sentences = sentencesOf(++place.spineIndex);
        place.index = spare;
    }
    while (place.sentences && place.index < 0) {
        place.sentences = sentencesOf(--place.spineIndex);
        if (place.sentences)
            place.index += int(place.sentences->sentences().size());
    }
    if (!place.sentences)
        return {};
    return place;
}

void BookSpeech::start(int spineIndex, const QString &cfi)
{
    if (!m_book || !ensureEngine())
        return;
    m_error.clear();
    Place place{ sentencesOf(spineIndex), spineIndex, 0 };
    if (!place.sentences)
        return;
    place.index = place.sentences->sentenceAt(cfi);
    place = moved(place, 0);
    if (!place.sentences) {
        stop();
        return;
    }
    setMode(QStringLiteral("speaking"));
    speak(place);
}

void BookSpeech::pause()
{
    if (!m_tts || m_mode != QLatin1String("speaking"))
        return;
    m_tts->pause();
    setMode(QStringLiteral("paused"));
}

void BookSpeech::resume()
{
    if (!m_tts || m_mode != QLatin1String("paused"))
        return;
    setMode(QStringLiteral("speaking"));
    // Engines pause at the end of a word: the sentence may have ended
    // instead, and there is nothing to resume.
    if (m_tts->state() != QTextToSpeech::Ready) {
        m_tts->resume();
        return;
    }
    const Place next = moved(m_current, 1);
    if (next.sentences)
        speak(next);
    else
        stop();
}

void BookSpeech::stop()
{
    silence();
    const bool failed = !m_error.isEmpty();
    m_error.clear();
    setMode(QStringLiteral("stopped"));
    if (failed)
        emit stateChanged();
}

void BookSpeech::skip(int count)
{
    if (m_mode == QLatin1String("stopped") || !m_current.sentences)
        return;
    const Place target = moved(m_current, count);
    if (!target.sentences) {
        stop();
        return;
    }
    setMode(QStringLiteral("speaking"));
    speak(target);
}

void BookSpeech::speak(const Place &place)
{
    // What was being said, stopped first: its being ready then is no end of
    // what is said now.
    if (m_tts->state() != QTextToSpeech::Ready) {
        m_stopping = true;
        m_tts->stop(QTextToSpeech::BoundaryHint::Immediate);
        m_stopping = false;
    }
    m_current = place;
    m_word.clear();
    emit sentenceChanged();
    emit wordChanged();
    m_starting = true;
    m_tts->say(place.sentences->sentences().at(place.index).text);
}

void BookSpeech::silence()
{
    const bool had = m_current.sentences != nullptr;
    m_current = {};
    m_word.clear();
    m_starting = false;
    if (m_tts) {
        m_stopping = true;
        m_tts->stop(QTextToSpeech::BoundaryHint::Immediate);
        m_stopping = false;
    }
    if (had) {
        emit sentenceChanged();
        emit wordChanged();
    }
}

void BookSpeech::setMode(const QString &mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    emit stateChanged();
}
