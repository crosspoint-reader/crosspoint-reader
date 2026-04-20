#include "BookFusionBookIdStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <cstdio>
#include <cstring>

void BookFusionBookIdStore::buildSidecarPath(const char* epubPath, char* outPath, size_t maxLen) {
  MD5Builder md5;
  md5.begin();
  md5.add(epubPath);
  md5.calculate();

  // Result: /.crosspoint/bookfusion_<32hexchars>.json  (55 chars total)
  snprintf(outPath, maxLen, "/.crosspoint/bookfusion_%s.json", md5.toString().c_str());
}

uint32_t BookFusionBookIdStore::loadBookId(const char* epubPath) {
  char sidecarPath[64];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));

  if (!Storage.exists(sidecarPath)) {
    return 0;
  }

  String json = Storage.readFile(sidecarPath);
  if (json.isEmpty()) {
    return 0;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("BFS", "Sidecar JSON parse error: %s", sidecarPath);
    return 0;
  }

  const uint32_t bookId = doc["book_id"] | (uint32_t)0;
  LOG_DBG("BFS", "Loaded book_id=%lu for %s", (unsigned long)bookId, epubPath);
  return bookId;
}

bool BookFusionBookIdStore::saveBookId(const char* epubPath, uint32_t bookId) {
  if (bookId == 0) {
    LOG_ERR("BFS", "Refusing to save book_id=0 for %s", epubPath);
    return false;
  }

  char sidecarPath[64];
  buildSidecarPath(epubPath, sidecarPath, sizeof(sidecarPath));

  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["book_id"] = bookId;

  String json;
  serializeJson(doc, json);

  const bool ok = Storage.writeFile(sidecarPath, json);
  if (ok) {
    LOG_DBG("BFS", "Saved book_id=%lu for %s", (unsigned long)bookId, epubPath);
  } else {
    LOG_ERR("BFS", "Failed to save sidecar: %s", sidecarPath);
  }
  return ok;
}
