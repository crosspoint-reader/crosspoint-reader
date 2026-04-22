#include "ClippingsManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

bool ClippingsManager::saveClipping(const std::string& bookTitle, const std::string& author,
                                    const std::string& chapterTitle, int pageNumber,
                                    const std::string& selectedText) {
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

  // Body: quoted text, trimmed to 2000 chars to avoid writing huge pages
  static constexpr size_t MAX_TEXT = 2000;
  const size_t textLen = selectedText.size() < MAX_TEXT ? selectedText.size() : MAX_TEXT;

  char quote[8];
  snprintf(quote, sizeof(quote), "\n\"");

  char separator[] = "\"\n\n==========\n\n";

  file.write(header, strlen(header));
  file.write(location, strlen(location));
  file.write(quote, strlen(quote));
  file.write(selectedText.c_str(), textLen);
  file.write(separator, strlen(separator));
  file.flush();
  file.close();

  LOG_DBG("CLIP", "Saved clipping to %s (%zu chars)", CLIPPINGS_PATH, textLen);
  return true;
}
