#include "ClippingsManager.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <common/FsApiConstants.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {
constexpr const char* WEEKDAY_NAMES[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
constexpr const char* MONTH_NAMES[] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};

bool formatKindleAddedOn(char* buf, const size_t bufSize) {
  if (!buf || bufSize == 0) return false;
  struct tm local;
  if (!halClock.localTime(local) || local.tm_year < 100 || local.tm_year > 199) return false;

  int hour12 = local.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;

  const int weekdayIndex = (local.tm_wday + 6) % 7;
  snprintf(buf, bufSize, "Added on %s, %s %u, %u, %02d:%02d %s", WEEKDAY_NAMES[weekdayIndex], MONTH_NAMES[local.tm_mon],
           static_cast<unsigned>(local.tm_mday), static_cast<unsigned>(local.tm_year + 1900), hour12, local.tm_min,
           local.tm_hour >= 12 ? "PM" : "AM");
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
  const size_t originalSize = file.fileSize();

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
  const size_t rawTextLen = std::min(selectedText.size(), MAX_TEXT_BYTES);
  const size_t textLen = static_cast<size_t>(utf8SafeTruncateBuffer(selectedText.data(), rawTextLen));
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
  const bool rolledBack = ok || file.truncate(originalSize);
  file.flush();

  if (!ok) {
    if (!rolledBack) LOG_ERR("CLIP", "Failed to roll back partial write to %s", CLIPPINGS_PATH);
    LOG_ERR("CLIP", "Failed to write clipping to %s", CLIPPINGS_PATH);
    return false;
  }

  return true;
}
