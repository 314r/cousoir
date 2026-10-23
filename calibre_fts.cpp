#include "calibre_fts.h"

// Ported from Calibre 9.16, src/calibre/db/sqlite_extension.cpp, Copyright
// (C) 2021 Kovid Goyal, GPL v3: the Tokenizer, its UTF-8 decoding, its script
// blocks and break iterators, its stemmer and its registration.

#include <QObject>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>
#include <libstemmer.h>
#include <unicode/brkiter.h>
#include <unicode/errorcode.h>
#include <unicode/translit.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uscript.h>

namespace {

using TokenCallback = int (*)(void *, int, const char *, int, int, int);

// Converting SQLite's text to ICU strings, with each character's byte offset:
// the UTF-8 decoder of https://bjoern.hoehrmann.de/utf-8/decoder/dfa/, as
// Calibre has it.
const uint8_t utf8Data[] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    8,   8,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    0xa, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x4, 0x3, 0x3,
    0xb, 0x6, 0x6, 0x6, 0x5, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8,
    0x0, 0x1, 0x2, 0x3, 0x5, 0x8, 0x7, 0x1, 0x1, 0x1, 0x4, 0x6, 0x1, 0x1, 0x1, 0x1,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1,
    1,   2,   1,   1,   1,   1,   1,   2,   1,   2,   1,   1,   1,   1,   1,   1,   1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    1,   2,   1,   1,   1,   1,   1,   1,   1,   2,   1,   1,   1,   1,   1,   1,   1, 1, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 1, 1, 1,
    1,   3,   1,   1,   1,   1,   1,   3,   1,   3,   1,   1,   1,   1,   1,   1,   1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

enum Utf8State : uint32_t { Utf8Accept = 0, Utf8Reject = 1 };

uint32_t decodeUtf8(uint32_t *state, uint32_t *codepoint, uint8_t byte)
{
    const uint32_t type = utf8Data[byte];
    *codepoint = (*state != Utf8Accept) ? (byte & 0x3fu) | (*codepoint << 6) : (0xffu >> type) & byte;
    *state = utf8Data[256 + *state * 16 + type];
    return *state;
}

void populateIcuString(const char *text, int size, icu::UnicodeString &str, std::vector<int> &byteOffsets)
{
    uint32_t state = Utf8Accept;
    uint32_t previous = Utf8Accept;
    uint32_t codepoint = 0;
    for (int i = 0, position = 0; i < size; i++) {
        switch (decodeUtf8(&state, &codepoint, uint8_t(text[i]))) {
        case Utf8Accept: {
            const int32_t before = str.length();
            str.append(UChar32(codepoint));
            for (int32_t x = 0; x < str.length() - before; x++)
                byteOffsets.push_back(position);
            position = i + 1;
            break;
        }
        case Utf8Reject:
            state = Utf8Accept;
            if (previous != Utf8Accept && i > 0)
                i--;
            break;
        default:
            break;
        }
        previous = state;
    }
    byteOffsets.push_back(size);
}

struct IteratorDescription
{
    const char *language;
    UScriptCode script;
};

class Stemmer
{
public:
    Stemmer() = default;
    explicit Stemmer(const char *language)
    {
        std::string name;
        for (const char *p = language; *p && name.size() < 31; ++p)
            name.push_back(('A' <= *p && *p <= 'Z') ? char(*p + ('a' - 'A')) : *p);
        m_handle = sb_stemmer_new(name.c_str(), nullptr);
    }
    ~Stemmer()
    {
        if (m_handle)
            sb_stemmer_delete(m_handle);
    }
    Stemmer(const Stemmer &) = delete;
    Stemmer &operator=(const Stemmer &) = delete;

    const char *stem(const char *token, size_t size, int *stemmedSize)
    {
        *stemmedSize = 0;
        if (!m_handle)
            return nullptr;
        const char *stemmed
            = reinterpret_cast<const char *>(sb_stemmer_stem(m_handle, reinterpret_cast<const sb_symbol *>(token), int(size)));
        if (stemmed)
            *stemmedSize = sb_stemmer_length(m_handle);
        return stemmed;
    }

    explicit operator bool() const { return m_handle != nullptr; }

private:
    sb_stemmer *m_handle = nullptr;
};

using BreakIterator = std::unique_ptr<icu::BreakIterator>;
using StemmerPtr = std::unique_ptr<Stemmer>;
const std::string emptyString;

// What a registration gives each tokenizer it makes: the interface language.
struct Context
{
    std::string language;
};

class Tokenizer
{
public:
    int constructorError = SQLITE_OK;

    Tokenizer(const char **args, int count, bool stemWords, std::string uiLanguage)
        : m_stemWords(stemWords), m_uiLanguage(std::move(uiLanguage))
    {
        for (int i = 0; i < count; i++) {
            if (std::strcmp(args[i], "remove_diacritics") == 0) {
                i++;
                if (i < count && std::strcmp(args[i], "0") == 0)
                    m_removeDiacritics = false;
            } else if (std::strcmp(args[i], "stem_words") == 0) {
                i++;
                m_stemWords = !(i < count && std::strcmp(args[i], "0") == 0);
            }
        }
        if (m_removeDiacritics) {
            icu::ErrorCode status;
            m_diacriticsRemover.reset(icu::Transliterator::createInstance("NFD; [:M:] Remove; NFC", UTRANS_FORWARD, status));
            if (status.isFailure()) {
                constructorError = SQLITE_INTERNAL;
                m_diacriticsRemover.reset();
                m_removeDiacritics = false;
            }
        }
    }

    int tokenize(void *callbackContext, int flags, const char *text, int size, TokenCallback callback)
    {
        ensureBasicIterator();
        m_callback = callback;
        m_callbackContext = callbackContext;
        icu::UnicodeString str(size, 0, 0);
        m_byteOffsets.clear();
        m_byteOffsets.reserve(size_t(size) + 8);
        populateIcuString(text, size, str, m_byteOffsets);
        int32_t offset = str.getChar32Start(0);
        int rc = SQLITE_OK;
        const bool forQueryOrAux = (flags & (FTS5_TOKENIZE_QUERY | FTS5_TOKENIZE_AUX)) != 0;
        IteratorDescription state{ "", USCRIPT_COMMON };
        int32_t blockStart = offset;
        BreakIterator *wordIterator = &ensureLanguageIterator(state.language);
        StemmerPtr *stemmer = &ensureStemmer(state.language);
        while (offset < str.length()) {
            const UChar32 ch = str.char32At(offset);
            if (atScriptBoundary(state, ch)) {
                if (offset > blockStart) {
                    if ((rc = tokenizeScriptBlock(str, blockStart, offset, forQueryOrAux, *wordIterator, *stemmer)) != SQLITE_OK)
                        return rc;
                }
                blockStart = offset;
                wordIterator = &ensureLanguageIterator(state.language);
                stemmer = &ensureStemmer(state.language);
            }
            offset = str.moveIndex32(offset, 1);
        }
        if (offset > blockStart)
            rc = tokenizeScriptBlock(str, blockStart, offset, forQueryOrAux, *wordIterator, *stemmer);
        return rc;
    }

private:
    static bool isTokenChar(UChar32 ch)
    {
        switch (u_charType(ch)) {
        case U_UPPERCASE_LETTER:
        case U_LOWERCASE_LETTER:
        case U_TITLECASE_LETTER:
        case U_MODIFIER_LETTER:
        case U_OTHER_LETTER:
        case U_DECIMAL_DIGIT_NUMBER:
        case U_LETTER_NUMBER:
        case U_OTHER_NUMBER:
        case U_CURRENCY_SYMBOL:
        case U_OTHER_SYMBOL:
        case U_PRIVATE_USE_CHAR:
            return true;
        default:
            return false;
        }
    }

    int sendToken(const icu::UnicodeString &token, int32_t start, int32_t end, StemmerPtr &stemmer, int flags = 0)
    {
        m_tokenBuffer.clear();
        m_tokenBuffer.reserve(4 * size_t(token.length()));
        token.toUTF8String(m_tokenBuffer);
        const char *root = m_tokenBuffer.c_str();
        int size = int(m_tokenBuffer.size());
        if (m_stemWords && *stemmer) {
            root = stemmer->stem(root, size_t(size), &size);
            if (!root) {
                root = m_tokenBuffer.c_str();
                size = int(m_tokenBuffer.size());
            }
        }
        return m_callback(m_callbackContext, flags, root, size, m_byteOffsets.at(size_t(start)), m_byteOffsets.at(size_t(end)));
    }

    static const char *iteratorLanguageForScript(UScriptCode script)
    {
        switch (script) {
        case USCRIPT_THAI:
        case USCRIPT_LAO:
            return "th_TH";
        case USCRIPT_KHMER:
            return "km_KH";
        case USCRIPT_MYANMAR:
            return "my_MM";
        case USCRIPT_HIRAGANA:
        case USCRIPT_KATAKANA:
            return "ja_JP";
        case USCRIPT_HANGUL:
            return "ko_KR";
        case USCRIPT_HAN:
        case USCRIPT_SIMPLIFIED_HAN:
        case USCRIPT_TRADITIONAL_HAN:
        case USCRIPT_HAN_WITH_BOPOMOFO:
            return "zh";
        default:
            return "";
        }
    }

    static bool atScriptBoundary(IteratorDescription &current, UChar32 next)
    {
        icu::ErrorCode error;
        const UScriptCode script = uscript_getScript(next, error);
        if (script == USCRIPT_COMMON || script == USCRIPT_INVALID_CODE || script == USCRIPT_INHERITED || current.script == script)
            return false;
        const char *language = iteratorLanguageForScript(script);
        if (std::strcmp(current.language, language) == 0)
            return false;
        current.script = script;
        current.language = language;
        return true;
    }

    void ensureBasicIterator()
    {
        if (m_iterators.find(emptyString) == m_iterators.end()) {
            icu::ErrorCode status;
            if (m_uiLanguage.empty())
                m_iterators[emptyString] = BreakIterator(icu::BreakIterator::createWordInstance(icu::Locale::getDefault(), status));
            else
                ensureLanguageIterator(m_uiLanguage.c_str());
        }
    }

    BreakIterator &ensureLanguageIterator(const char *language)
    {
        auto found = m_iterators.find(language);
        if (found == m_iterators.end()) {
            icu::ErrorCode status;
            m_iterators[language] = BreakIterator(icu::BreakIterator::createWordInstance(icu::Locale::createCanonical(language), status));
            if (status.isFailure())
                m_iterators[language] = BreakIterator(icu::BreakIterator::createWordInstance(icu::Locale::getDefault(), status));
            found = m_iterators.find(language);
        }
        return found->second;
    }

    StemmerPtr &ensureStemmer(const char *language)
    {
        if (!language[0])
            language = m_uiLanguage.c_str();
        auto found = m_stemmers.find(language);
        if (found == m_stemmers.end()) {
            m_stemmers[language] = m_stemWords ? std::make_unique<Stemmer>(language) : std::make_unique<Stemmer>();
            found = m_stemmers.find(language);
        }
        return found->second;
    }

    int tokenizeScriptBlock(const icu::UnicodeString &str, int32_t blockStart, int32_t blockLimit, bool forQueryOrAux,
                            BreakIterator &wordIterator, StemmerPtr &stemmer)
    {
        wordIterator->setText(str.tempSubStringBetween(blockStart, blockLimit));
        int32_t tokenStart = wordIterator->first() + blockStart;
        int32_t tokenEnd = 0;
        int rc = SQLITE_OK;
        do {
            tokenEnd = wordIterator->next();
            if (tokenEnd == icu::BreakIterator::DONE)
                tokenEnd = blockLimit;
            else
                tokenEnd += blockStart;
            if (tokenEnd > tokenStart) {
                bool isToken = false;
                for (int32_t position = tokenStart; !isToken && position < tokenEnd; position = str.moveIndex32(position, 1)) {
                    if (isTokenChar(str.char32At(position)))
                        isToken = true;
                }
                if (isToken) {
                    icu::UnicodeString token(str, tokenStart, tokenEnd - tokenStart);
                    token.foldCase();
                    if ((rc = sendToken(token, tokenStart, tokenEnd, stemmer)) != SQLITE_OK)
                        return rc;
                    if (!forQueryOrAux && m_removeDiacritics) {
                        icu::UnicodeString stripped(str, tokenStart, tokenEnd - tokenStart);
                        m_diacriticsRemover->transliterate(stripped);
                        stripped.foldCase();
                        if (stripped != token) {
                            if ((rc = sendToken(stripped, tokenStart, tokenEnd, stemmer, FTS5_TOKEN_COLOCATED)) != SQLITE_OK)
                                return rc;
                        }
                    }
                }
            }
            tokenStart = tokenEnd;
        } while (tokenEnd < blockLimit);
        return rc;
    }

    bool m_removeDiacritics = true;
    bool m_stemWords = false;
    std::unique_ptr<icu::Transliterator> m_diacriticsRemover;
    std::vector<int> m_byteOffsets;
    std::string m_tokenBuffer;
    std::string m_uiLanguage;
    TokenCallback m_callback = nullptr;
    void *m_callbackContext = nullptr;
    std::unordered_map<std::string, BreakIterator> m_iterators;
    std::unordered_map<std::string, StemmerPtr> m_stemmers;
};

int create(void *context, const char **args, int count, Fts5Tokenizer **out, bool stemWords)
{
    try {
        auto *tokenizer = new Tokenizer(args, count, stemWords, static_cast<Context *>(context)->language);
        if (tokenizer->constructorError != SQLITE_OK) {
            const int rc = tokenizer->constructorError;
            delete tokenizer;
            return rc;
        }
        *out = reinterpret_cast<Fts5Tokenizer *>(tokenizer);
        return SQLITE_OK;
    } catch (const std::bad_alloc &) {
        return SQLITE_NOMEM;
    } catch (...) {
        return SQLITE_ERROR;
    }
}

int createPlain(void *context, const char **args, int count, Fts5Tokenizer **out)
{
    return create(context, args, count, out, false);
}

int createStemming(void *context, const char **args, int count, Fts5Tokenizer **out)
{
    return create(context, args, count, out, true);
}

int tokenize(Fts5Tokenizer *tokenizer, void *callbackContext, int flags, const char *text, int size, TokenCallback callback)
{
    try {
        return reinterpret_cast<Tokenizer *>(tokenizer)->tokenize(callbackContext, flags, text, size, callback);
    } catch (const std::bad_alloc &) {
        return SQLITE_NOMEM;
    } catch (...) {
        return SQLITE_ERROR;
    }
}

void destroy(Fts5Tokenizer *tokenizer)
{
    delete reinterpret_cast<Tokenizer *>(tokenizer);
}

fts5_api *fts5Api(sqlite3 *db)
{
    fts5_api *api = nullptr;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_pointer(statement, 1, reinterpret_cast<void *>(&api), "fts5_api_ptr", nullptr);
        sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    return api;
}

} // namespace

namespace CalibreFts {

bool registerTokenizers(sqlite3 *db, const QString &interfaceLanguage, QString *error)
{
    fts5_api *api = fts5Api(db);
    if (!api || api->iVersion < 2) {
        if (error)
            *error = QObject::tr("SQLite was built without full text search (FTS5).");
        return false;
    }
    // bcp47_locale_name(): the language as it is, but for a script given as
    // a modifier, "sr@latin" as "sr_Latn".
    QString language = interfaceLanguage;
    if (language.contains(u'@')) {
        const QString modifier = language.section(u'@', 1).toLower();
        language = language.section(u'@', 0, 0);
        if (modifier == QLatin1String("latin"))
            language += QStringLiteral("_Latn");
        else if (modifier == QLatin1String("cyrillic"))
            language += QStringLiteral("_Cyrl");
    }
    auto *context = new Context{ language.toStdString() };
    const auto release = [](void *context) { delete static_cast<Context *>(context); };
    fts5_tokenizer plain{ createPlain, destroy, tokenize };
    fts5_tokenizer stemming{ createStemming, destroy, tokenize };
    // Each registration owns a context of its own.
    api->xCreateTokenizer(api, "unicode61", context, &plain, release);
    api->xCreateTokenizer(api, "calibre", new Context(*context), &plain, release);
    api->xCreateTokenizer(api, "porter", new Context(*context), &stemming, release);
    return true;
}

} // namespace CalibreFts
