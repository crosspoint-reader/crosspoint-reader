#include "PanelCurve.h"

#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "CrossPointSettings.h"

namespace panelcurve {
namespace {

uint32_t fnv1a(const uint8_t* data) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 256; ++i) h = (h ^ data[i]) * 16777619u;
  return h;
}

constexpr uint8_t IDENTITY[256] = {
#define R16(b) \
  b, b + 1, b + 2, b + 3, b + 4, b + 5, b + 6, b + 7, b + 8, b + 9, b + 10, b + 11, b + 12, b + 13, b + 14, b + 15
    R16(0),   R16(16),  R16(32),  R16(48),  R16(64),  R16(80),  R16(96),  R16(112),
    R16(128), R16(144), R16(160), R16(176), R16(192), R16(208), R16(224), R16(240),
#undef R16
};

struct Source {
  const char* rootPath;
  const char* hiddenPath;
};
constexpr Source PICTURES{ROOT_PATH, PATH};
constexpr Source BOOKS{BOOK_ROOT_PATH, BOOK_PATH};

void useIdentity(uint8_t table[256], uint32_t& hash) {
  for (int i = 0; i < 256; ++i) table[i] = IDENTITY[i];
  hash = fnv1a(IDENTITY);
}

// Prefer the configuration directory, retaining root files as a read-only legacy fallback.
void readFromCard(const Source& src, uint8_t table[256], uint32_t& hash) {
  const char* path = Storage.exists(src.hiddenPath) ? src.hiddenPath : src.rootPath;
  std::string text;
  if (!Storage.exists(path) || !Storage.readFileToString("CRV", path, 4096, text)) return useIdentity(table, hash);

  uint8_t parsed[256];
  int count = 0;
  int value = -1;
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value * 10) + (c - '0');
      if (value > 255) {
        LOG_ERR("CRV", "Value above 255 in %s", path);
        return useIdentity(table, hash);
      }
    } else if (c != ',' && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      LOG_ERR("CRV", "Invalid character in %s", path);
      return useIdentity(table, hash);
    } else if (value >= 0) {
      if (count == 256) {
        LOG_ERR("CRV", "More than 256 values in %s", path);
        return useIdentity(table, hash);
      }
      parsed[count++] = static_cast<uint8_t>(value);
      value = -1;
    }
  }
  if (value >= 0) {
    if (count == 256) {
      LOG_ERR("CRV", "More than 256 values in %s", path);
      return useIdentity(table, hash);
    }
    parsed[count++] = static_cast<uint8_t>(value);
  }
  if (count != 256) {
    LOG_ERR("CRV", "%s has %d values, expected 256", path, count);
    return useIdentity(table, hash);
  }
  for (int i = 0; i < 256; ++i) table[i] = parsed[i];
  hash = fnv1a(parsed);
  LOG_DBG("CRV", "Curve loaded from %s: %08x", path, static_cast<unsigned>(hash));
}

// Each curve is 256 bytes: read from the card on first use, then kept in RAM until a checkbox is switched (forget()).
// The checkboxes are read on every call, so switching one takes effect at once.
struct Cached {
  uint8_t table[256];
  uint32_t hash = 0;
  bool ready = false;
};
const Cached& cached(const Source& src, Cached& slot) {
  if (!slot.ready) {
    readFromCard(src, slot.table, slot.hash);
    slot.ready = true;
  }
  return slot;
}
Cached picturesSlot;
Cached booksSlot;
}  // namespace

bool load(uint8_t table[256], uint32_t& hash) {
  if (!SETTINGS.panelCalPictures) {
    for (int i = 0; i < 256; ++i) table[i] = IDENTITY[i];
    hash = fnv1a(IDENTITY);
    return true;
  }
  const Cached& c = cached(PICTURES, picturesSlot);
  for (int i = 0; i < 256; ++i) table[i] = c.table[i];
  hash = c.hash;
  return true;
}

void forget() {
  picturesSlot.ready = false;
  booksSlot.ready = false;
}

const uint8_t* bookCurve(uint32_t& hash) {
  if (!SETTINGS.panelCalBooks) {
    hash = 0;
    return nullptr;
  }
  const Cached& c = cached(BOOKS, booksSlot);
  hash = c.hash;
  return c.table;
}

}  // namespace panelcurve
