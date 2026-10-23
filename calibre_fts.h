#pragma once

#include <QString>

struct sqlite3;

// Calibre's FTS5 tokenizer, which Calibre registers on its connections under
// the names "unicode61", "calibre" and "porter" (the last with Snowball
// stemming): ICU's word breaks in the interface language, case folded, each
// word with diacritics also given without them. The annotations and notes
// Calibre's search finds are indexed with it; whoever writes their rows must
// index them alike, or Calibre, removing one later, would remove words the
// index never had.
namespace CalibreFts {

// Registers the tokenizers on `db`, as Calibre's sqlite_extension does, for
// Calibre's `interfaceLanguage` ("fr", "pt_BR"). Returns false with `error`
// when SQLite has no FTS5.
bool registerTokenizers(sqlite3 *db, const QString &interfaceLanguage, QString *error);

} // namespace CalibreFts
