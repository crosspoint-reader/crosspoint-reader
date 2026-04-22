#include "ClippingsManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

bool ClippingsManager::saveClipping(const std::string& bookTitle, const std::string& author,
                                    const std::string& chapterTitle, int pageNumber, const std::string& selectedText) {
  HalFile file = Storage.open(CLIPPINGS_PATH, O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("CLIP", "Failed to open %s for append", CLIPPINGS_PATH);
    return false;
  }

  // Header line: "Title / Author"
  char header[128];
  snprintf(header, sizeof(header), "%s / %s\n", bookTitle.c_str(), author.c_str());

  // Location line: "Chapter: X | Page N"
  char location[128];
  if (!chapterTitle.empty()) {
    snprintf(location, sizeof(location), "Chapter: %s | Page %d\n", chapterTitle.c_str(), pageNumber);
  } else {
    snprintf(location, sizeof(location), "Page %d\n", pageNumber);
  }

  // Body: quoted text, trimmed to 2000 chars to avoid writing huge clippings
  static constexpr size_t MAX_TEXT = 2000;
  const size_t textLen = selectedText.size() < MAX_TEXT ? selectedText.size() : MAX_TEXT;

  static constexpr char quote[] = "\n\"";
  static constexpr char separator[] = "\"\n\n==========\n\n";

  const size_t headerLen = strlen(header);
  const size_t locationLen = strlen(location);
  const size_t quoteLen = strlen(quote);
  const size_t separatorLen = strlen(separator);

  bool ok = file.write(header, headerLen) == headerLen;
  ok = ok && file.write(location, locationLen) == locationLen;
  ok = ok && file.write(quote, quoteLen) == quoteLen;
  ok = ok && file.write(selectedText.c_str(), textLen) == textLen;
  ok = ok && file.write(separator, separatorLen) == separatorLen;
  file.flush();
  file.close();

  if (!ok) {
    LOG_ERR("CLIP", "Failed to write clipping to %s (SD full or removed?)", CLIPPINGS_PATH);
    return false;
  }

  LOG_DBG("CLIP", "Saved clipping to %s (%zu chars)", CLIPPINGS_PATH, textLen);
  return true;
}
