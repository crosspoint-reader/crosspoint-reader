#pragma once

#include <cstdint>
#include <string>

class SyncRequest;

// Append-only history of measured reading sessions on the SD card
// (/.crosspoint/reading_sessions.jsonl, one JSON object per line):
//   doc    32-hex sync document id      start  Unix time of the first counted page
//   secs   active reading seconds       tz     UTC offset in minutes at the start
//   from/to  whole-book progress in basis points (0..10000)
// The file is never trimmed, so the full history can be uploaded to a new account.
namespace readingSessionLog {

bool append(const std::string& document, int64_t startTime, uint32_t seconds, uint16_t startProgressBp,
            uint16_t endProgressBp);

// Uploads sessions the configured account has not acknowledged, oldest first.
// One call sends a bounded number; the rest follow on later syncs.
bool upload(SyncRequest& sync);

}  // namespace readingSessionLog
