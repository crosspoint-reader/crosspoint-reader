#pragma once

#include <cstdint>
#include <string>

namespace AppDateTimeFormat {

// Returns UTC ISO-8601 timestamp (YYYY-MM-DDTHH:MM:SSZ) or empty when system clock is unset.
std::string formatNowIso8601Utc();

// Formats a UTC ISO-8601 timestamp for display using the user's clock offset and 12/24h preference.
// Returns empty when isoUtc is empty or unparseable.
std::string formatIso8601ForDisplay(const std::string& isoUtc, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour);

// Lexicographic compare for descending sort (newest first). Empty strings sort last.
bool isNewerInstalledAt(const std::string& a, const std::string& b);

}  // namespace AppDateTimeFormat
