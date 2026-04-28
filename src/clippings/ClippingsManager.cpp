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

  // Build header and location as strings to avoid truncation of long titles/authors
  const std::string header = bookTitle + " (" + author + ")\n";
  std::string location = "- Your Highlight on Page " + std::to_string(pageNumber);
  if (!chapterTitle.empty()) {
    location += " | " + chapterTitle;
  }
  location += "\n";

  static constexpr size_t MAX_TEXT = 2000;
  const size_t textLen = selectedText.size() < MAX_TEXT ? selectedText.size() : MAX_TEXT;

  // Concatenate into a single buffer and perform one write to avoid partial records on SD failure
  static constexpr char separator[] = "\n==========\n";
  static constexpr size_t separatorLen = sizeof(separator) - 1;
  const size_t totalLen = header.size() + location.size() + 1 + textLen + separatorLen;

  auto* buf = static_cast<char*>(malloc(totalLen));
  if (!buf) {
    LOG_ERR("CLIP", "malloc failed: %zu bytes", totalLen);
    file.close();
    return false;
  }

  size_t pos = 0;
  memcpy(buf + pos, header.c_str(), header.size());
  pos += header.size();
  memcpy(buf + pos, location.c_str(), location.size());
  pos += location.size();
  buf[pos++] = '\n';
  memcpy(buf + pos, selectedText.c_str(), textLen);
  pos += textLen;
  memcpy(buf + pos, separator, separatorLen);

  const bool ok = file.write(buf, totalLen) == totalLen;
  free(buf);
  file.flush();
  file.close();

  if (!ok) {
    LOG_ERR("CLIP", "Failed to write clipping to %s (SD full or removed?)", CLIPPINGS_PATH);
    return false;
  }

  LOG_DBG("CLIP", "Saved clipping to %s (%zu chars)", CLIPPINGS_PATH, textLen);
  return true;
}
