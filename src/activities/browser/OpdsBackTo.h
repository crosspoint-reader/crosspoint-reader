#pragma once

#include <cstdint>

// Where Back from the top of the OPDS server picker or catalog goes: File Transfer, where both are
// opened from, or Home for a finished book's "more by this author" search, which never passed
// through File Transfer. A long Back goes Home either way.
enum class OpdsBackTo : uint8_t { FileTransfer, Home };
