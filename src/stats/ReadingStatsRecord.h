#pragma once

#include <cstddef>
#include <cstdint>

// Reading statistics records and their pure update rules. File access lives in
// ReadingStatsStore; this part has no I/O so host tests can cover it.
namespace readingstats {

// A reading session shorter than this still adds time and pages, but is not
// counted as a session or in the average session length.
inline constexpr uint32_t MIN_SESSION_SECONDS = 60;

struct Totals {
  uint32_t seconds = 0;
  uint32_t sessions = 0;
  // Reading time of the counted sessions only, for the average session length.
  uint32_t sessionSeconds = 0;
  uint32_t pages = 0;
  uint32_t finishedBooks = 0;
  // Local calendar day (days since 1970-01-01) of the latest reading; 0 = never.
  int32_t lastReadDay = 0;
  // Consecutive reading days ending on lastReadDay.
  uint16_t streakDays = 0;
  // Reading time on lastReadDay, for the daily goal.
  uint32_t lastDaySeconds = 0;
};

struct Book {
  uint32_t seconds = 0;
  uint32_t sessions = 0;
  uint32_t sessionSeconds = 0;  // as in Totals
  uint32_t pages = 0;
  bool finished = false;
};

// Adds one finished reading session to both records. localDay is the session's
// local calendar day, or 0 when the date is unknown (the streak is then left alone).
void applySession(Totals& totals, Book& book, uint32_t seconds, uint32_t pages, int32_t localDay);

// Marks or clears a book as finished, keeping the finished-books total in step.
// Returns false when the book was already in that state.
bool setFinished(Totals& totals, Book& book, bool finished);

// The streak as of today: it still counts when the latest reading day was
// yesterday, and drops to 0 after a day without reading.
uint16_t currentStreak(const Totals& totals, int32_t today);

// Reading time on the given local day: 0 when the latest reading was another day.
uint32_t secondsOnDay(const Totals& totals, int32_t day);

// Days since 1970-01-01 for a proleptic Gregorian date (month 1-12).
int32_t civilDay(int year, int month, int day);

// Fixed-size little-endian encodings with a magic, a version and a CRC-32.
inline constexpr size_t TOTALS_BYTES = 39;
inline constexpr size_t BOOK_BYTES = 26;

void encode(const Totals& totals, uint8_t (&out)[TOTALS_BYTES]);
void encode(const Book& book, uint8_t (&out)[BOOK_BYTES]);
// False for a wrong size, magic, version or checksum; out is left untouched.
bool decode(const uint8_t* data, size_t len, Totals& out);
bool decode(const uint8_t* data, size_t len, Book& out);

}  // namespace readingstats
