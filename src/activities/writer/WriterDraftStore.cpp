#include "WriterDraftStore.h"

#include <HalStorage.h>
#include <Logging.h>

// Check draft folder exists
bool WriterDraftStore::ensureDraft() {
  const std::string folderPath = DraftDir;
  const std::string draftPath = DraftPath;
  if (!Storage.exists(folderPath.c_str())) {
    // Create the folder
    if (!Storage.mkdir(folderPath.c_str())) {
      LOG_ERR("Writer", "Failed to create folder: %s", folderPath.c_str());
      return false;
    }
    LOG_DBG("Writer", "Folder created successfully: %s", folderPath.c_str());
  }

  if (!Storage.exists(draftPath.c_str())) {
    // Create the file
    HalFile file;
    if (!Storage.openFileForWrite("Writer", draftPath, file)) {
      LOG_ERR("Writer", "Failed to create file: %s", draftPath.c_str());
      return false;
    }
    LOG_DBG("Writer", "Draft file created successfully: %s", draftPath.c_str());
    file.close();
  }

  return true;
}

bool WriterDraftStore::readDraft(std::string& out) {
  out.clear();

  if (!ensureDraft()) {
    return false;
  }

  HalFile file;

  if (!Storage.openFileForRead("Writer", DraftPath, file)) {
    LOG_ERR("Writer", "Failed to open draft file: %s", DraftPath);
    return false;
  }

  constexpr size_t MaxDraftBytes = 16 * 1024;
  if (file.size() > MaxDraftBytes) {
    LOG_ERR("Writer", "Draft too large to load: %s (%zu bytes)", DraftPath, file.size());
    file.close();
    return false;
  }

  constexpr size_t bufferSize = 256;
  uint8_t buffer[bufferSize];

  while (file.available()) {
    const int bytesRead = file.read(buffer, bufferSize);
    if (bytesRead <= 0) {
      file.close();
      LOG_ERR("Writer", "Failed while reading draft file: %s", DraftPath);
      return false;
    }

    out.append(reinterpret_cast<const char*>(buffer), static_cast<size_t>(bytesRead));
  }

  file.close();
  LOG_DBG("Writer", "Read draft file: %s (%zu bytes)", DraftPath, out.size());
  return true;
}
