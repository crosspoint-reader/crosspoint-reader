#include "ClippingsManager.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

#include <algorithm>
#include <cstdio>

namespace {
constexpr const char* WEEKDAY_NAMES[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
constexpr const char* MONTH_NAMES[] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return DAYS[month - 1];
}

bool isValidDate(const uint16_t year, const uint8_t month, const uint8_t day) {
  if (year < 2000 || year > 2099) return false;
  const uint8_t monthDays = daysInMonth(year, month);
  return monthDays > 0 && day >= 1 && day <= monthDays;
}

uint32_t dayIndexSince2000(const uint16_t year, const uint8_t month, const uint8_t day) {
  uint32_t dayIndex = 0;
  for (uint16_t y = 2000; y < year; ++y) {
    dayIndex += isLeapYear(y) ? 366u : 365u;
  }
  for (uint8_t m = 1; m < month; ++m) {
    dayIndex += daysInMonth(year, m);
  }
  return dayIndex + static_cast<uint32_t>(day - 1);
}

bool formatKindleAddedOn(char* buf, const size_t bufSize) {
  if (!buf || bufSize == 0 || !halClock.isAvailable()) return false;

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute) || !isValidDate(year, month, day)) return false;

  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;

  // 2000-01-01 was a Saturday; convert to Monday = 0 for WEEKDAY_NAMES.
  const uint8_t weekdayIndex = static_cast<uint8_t>((5u + dayIndexSince2000(year, month, day)) % 7u);
  snprintf(buf, bufSize, "Added on %s, %s %u, %u, %02d:%02d %s", WEEKDAY_NAMES[weekdayIndex], MONTH_NAMES[month - 1],
           static_cast<unsigned>(day), static_cast<unsigned>(year), hour12, minute, hour >= 12 ? "PM" : "AM");
  return true;
}
}  // namespace

bool ClippingsManager::saveClipping(const std::string& bookTitle, const std::string& author,
                                    const std::string& chapterTitle, const int pageNumber,
                                    const std::string& selectedText) {
  HalFile file = Storage.open(CLIPPINGS_PATH, O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("CLIP", "Failed to open %s for append", CLIPPINGS_PATH);
    return false;
  }

  std::string location = "- Your Highlight on Page " + std::to_string(pageNumber);
  if (!chapterTitle.empty()) {
    location += " | " + chapterTitle;
  }
  char addedOn[80];
  if (formatKindleAddedOn(addedOn, sizeof(addedOn))) {
    location += " | ";
    location += addedOn;
  }
  location += "\n";

  static constexpr size_t MAX_TEXT_BYTES = 2000;
  const size_t textLen = std::min(selectedText.size(), MAX_TEXT_BYTES);
  static constexpr char separator[] = "\n==========\n";

  std::string buffer;
  buffer.reserve(bookTitle.size() + author.size() + location.size() + textLen + sizeof(separator) + 8);
  buffer += bookTitle;
  buffer += " (";
  buffer += author;
  buffer += ")\n";
  buffer += location;
  buffer += '\n';
  buffer.append(selectedText.c_str(), textLen);
  buffer += separator;

  const bool ok = file.write(buffer.data(), buffer.size()) == buffer.size();
  file.flush();
  file.close();

  if (!ok) {
    LOG_ERR("CLIP", "Failed to write clipping to %s", CLIPPINGS_PATH);
    return false;
  }

  return true;
}
