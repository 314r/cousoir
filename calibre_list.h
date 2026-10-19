#pragma once

#include <QByteArray>

#include "book.h"

namespace CalibreList {

// What `calibredb list --for-machine --fields all` prints for `data`, byte
// for byte: the books by id, each a JSON object of Calibre's fields with
// sorted keys, formatted by Python's json.dumps(indent=2, sort_keys=True).
// Fields without a value are left out, as calibredb leaves them.
QByteArray machineList(const LibraryData &data);

} // namespace CalibreList
