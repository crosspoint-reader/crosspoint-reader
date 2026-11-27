#pragma once

#include <cstdint>
#include <string>

#include "ReadingStatsRecord.h"

// Reading statistics on the SD card, under /.crosspoint/stats/:
//   totals.bin     all-time Totals
//   <id>.bin       one Book per book, where <id> is the MD5 of the book's file
//                  name, so stats survive a move to another folder and
//                  "Delete cache", but not a rename.
// Each save writes <file>.tmp and replaces the old file. A power cut between
// the replace's remove and rename leaves only the .tmp, which load falls back to.
namespace readingstats {

// Missing or unreadable files read as empty records.
Totals loadTotals();
Book loadBook(const std::string& bookPath);

// Adds a finished reading session to the book and the totals. startEpoch is the
// session's trusted start time; it dates the session for the day streak.
void recordSession(const std::string& bookPath, uint32_t seconds, uint32_t pages, int64_t startEpoch);

// Saves the book's finished state. Returns false when nothing could be saved.
bool setBookFinished(const std::string& bookPath, bool finished);

// Local calendar day for a Unix time (see civilDay), or 0 when the time is unset.
int32_t localDay(int64_t epoch);

}  // namespace readingstats
