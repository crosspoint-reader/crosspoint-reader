#include "HttpFileStreamer.h"

#include <Logging.h>
#include <esp_task_wdt.h>

namespace {
constexpr size_t DOWNLOAD_CHUNK_SIZE = 4096;
}

namespace HttpFileStreamer {
bool streamFileToClient(FsFile& file, NetworkClient& client) {
  auto* buffer = static_cast<uint8_t*>(malloc(DOWNLOAD_CHUNK_SIZE));
  if (!buffer) {
    LOG_ERR("HTTP", "malloc failed: %zu bytes", DOWNLOAD_CHUNK_SIZE);
    return false;
  }

  bool ok = true;
  while (ok) {
    esp_task_wdt_reset();
    int result = file.read(buffer, DOWNLOAD_CHUNK_SIZE);
    if (result < 0) {
      ok = false;
      break;
    }
    if (result == 0) break;

    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      esp_task_wdt_reset();
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        ok = false;
        break;
      }
      totalWritten += wrote;
    }
  }

  free(buffer);
  return ok;
}
}  // namespace HttpFileStreamer
