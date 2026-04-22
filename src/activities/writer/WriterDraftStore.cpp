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
  LOG_INF("Writer", "readDraft not implemented yet");
  return true;
}
