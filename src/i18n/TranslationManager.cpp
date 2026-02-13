#include "TranslationManager.h"

#include <Esp.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "LangKeys.inc"

static constexpr size_t INITIAL_POOL_SIZE = 4096;
static constexpr size_t MAX_LINE_LENGTH = 256;

// ──────────────────────────────────────────────
// Singleton
// ──────────────────────────────────────────────

TranslationManager& TranslationManager::getInstance() {
  static TranslationManager instance;
  return instance;
}

TranslationManager::TranslationManager() = default;

TranslationManager::~TranslationManager() { freeAll(); }

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

bool TranslationManager::init(const char* lang) {
  LOG_DBG("I18N", "Initializing");

  freeAll();

  // For English, no translations needed — T() returns the key itself.
  bool sdLoaded = true;
  if (strcmp(lang, "en") != 0) {
    if (isValidLangCode(lang)) {
      sdLoaded = loadFromSD(lang);
      if (!sdLoaded) {
        LOG_ERR("I18N", "Failed to load '%s', using English", lang);
      }
    } else {
      LOG_ERR("I18N", "Invalid language code '%s'", lang);
      sdLoaded = false;
    }
  }

  strncpy(currentLang, sdLoaded ? lang : "en", sizeof(currentLang) - 1);
  currentLang[sizeof(currentLang) - 1] = '\0';

  LOG_INF("I18N", "Active: %s (%u strings, %u bytes pool)", currentLang, loadedCount, static_cast<unsigned>(poolUsed));
  return sdLoaded;
}

const char* TranslationManager::getString(const char* key) const {
  // Fast path: English or not initialized — return the key itself.
  if (!pool) return key;

  const int16_t id = lookupId(fnv1a(key));
  if (id < 0) return key;  // Unknown key

  const uint16_t offset = valueOffsets[id];
  if (offset == 0xFFFF) return key;  // No translation for this key

  return pool + offset;
}

size_t TranslationManager::getMemoryUsage() const { return poolSize + (LANG_KEY_COUNT * sizeof(uint16_t)); }

// ──────────────────────────────────────────────
// Hash table lookup — O(1) amortized
// ──────────────────────────────────────────────

int16_t TranslationManager::lookupId(uint32_t hash) {
  if (hash == 0) return -1;  // 0 is the empty-slot marker

  uint16_t slot = hash % LANG_HASH_TABLE_SIZE;
  for (uint16_t i = 0; i < LANG_HASH_TABLE_SIZE; i++) {
    const auto& entry = LANG_HASH_TABLE[slot];
    if (entry.hash == 0) return -1;                    // Empty slot = not found
    if (entry.hash == hash) return (int16_t)entry.id;  // Found
    slot = (slot + 1) % LANG_HASH_TABLE_SIZE;          // Linear probe
  }
  return -1;  // Table full (should never happen)
}

// ──────────────────────────────────────────────
// Language scanning
// ──────────────────────────────────────────────

const std::vector<TranslationManager::LangInfo>& TranslationManager::getAvailableLanguages() {
  if (languagesScanned) {
    return availableLanguages;
  }

  const unsigned long scanStart = millis();
  LOG_DBG("I18N", "Scanning languages, free heap: %u", static_cast<unsigned>(ESP.getFreeHeap()));

  availableLanguages.clear();

  // English is always available (built-in fallback).
  LangInfo english{};
  strncpy(english.code, "en", sizeof(english.code));
  strncpy(english.name, "English", sizeof(english.name));
  availableLanguages.push_back(english);

  // Scan /config/lang/ directory on SD card.
  FsFile dir = Storage.open("/config/lang");
  if (!dir) {
    LOG_ERR("I18N", "Cannot open /config/lang directory");
  } else if (!dir.isDirectory()) {
    LOG_ERR("I18N", "/config/lang is not a directory");
    dir.close();
  } else {
    int filesScanned = 0;
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }

      char filename[32];
      file.getName(filename, sizeof(filename));
      filesScanned++;

      // Must end in .lang
      const size_t nameLen = strlen(filename);
      if (nameLen < 6 || strcmp(filename + nameLen - 5, ".lang") != 0) {
        file.close();
        continue;
      }

      // Extract language code from filename (e.g. "cs.lang" -> "cs")
      char code[8] = {};
      const size_t codeLen = nameLen - 5;
      if (codeLen < 2 || codeLen > 7) {
        file.close();
        continue;
      }
      memcpy(code, filename, codeLen);
      code[codeLen] = '\0';

      // Skip "en" — already added.
      if (strcmp(code, "en") == 0) {
        file.close();
        continue;
      }

      if (!isValidLangCode(code)) {
        LOG_DBG("I18N", "Skipping invalid lang code: %s", filename);
        file.close();
        continue;
      }

      // Read first ~20 lines looking for language.name=...
      char displayName[64] = {};
      char lineBuf[MAX_LINE_LENGTH];
      int linesRead = 0;
      while (file.available() && linesRead < 20) {
        const int bytesRead = file.fgets(lineBuf, sizeof(lineBuf));
        if (bytesRead <= 0) break;
        linesRead++;

        // Strip newline
        int len = bytesRead;
        while (len > 0 && (lineBuf[len - 1] == '\n' || lineBuf[len - 1] == '\r')) {
          lineBuf[--len] = '\0';
        }

        if (len == 0 || lineBuf[0] == '#') continue;

        // Check for language.name=
        if (strncmp(lineBuf, "language.name=", 14) == 0) {
          strncpy(displayName, lineBuf + 14, sizeof(displayName) - 1);
          displayName[sizeof(displayName) - 1] = '\0';
          break;
        }
      }
      file.close();

      // If no display name found, use the code itself.
      if (displayName[0] == '\0') {
        strncpy(displayName, code, sizeof(displayName) - 1);
      }

      LOG_DBG("I18N", "Found language: %s (%s)", displayName, code);

      LangInfo info{};
      strncpy(info.code, code, sizeof(info.code));
      strncpy(info.name, displayName, sizeof(info.name));
      availableLanguages.push_back(info);
    }
    dir.close();
    LOG_DBG("I18N", "Scanned %d files in /config/lang", filesScanned);
  }

  // Sort non-English entries alphabetically by display name.
  if (availableLanguages.size() > 1) {
    std::sort(availableLanguages.begin() + 1, availableLanguages.end(),
              [](const LangInfo& a, const LangInfo& b) { return strcmp(a.name, b.name) < 0; });
  }

  languagesScanned = true;
  LOG_INF("I18N", "Found %u languages in %lu ms, free heap: %u", static_cast<unsigned>(availableLanguages.size()),
          millis() - scanStart, static_cast<unsigned>(ESP.getFreeHeap()));
  return availableLanguages;
}

std::vector<std::string> TranslationManager::getAvailableLanguageNames() {
  const auto& langs = getAvailableLanguages();
  std::vector<std::string> names;
  names.reserve(langs.size());
  std::transform(langs.begin(), langs.end(), std::back_inserter(names),
                 [](const LangInfo& lang) { return std::string(lang.name); });
  return names;
}

uint8_t TranslationManager::getCurrentLanguageIndex() {
  const auto& langs = getAvailableLanguages();
  for (size_t i = 0; i < langs.size(); i++) {
    if (strcmp(langs[i].code, currentLang) == 0) {
      return static_cast<uint8_t>(i);
    }
  }
  return 0;  // Default to English
}

// ──────────────────────────────────────────────
// Memory management
// ──────────────────────────────────────────────

void TranslationManager::freeAll() {
  free(pool);
  pool = nullptr;
  poolSize = 0;
  poolUsed = 0;

  free(valueOffsets);
  valueOffsets = nullptr;

  loadedCount = 0;
}

// ──────────────────────────────────────────────
// SD card loader
// ──────────────────────────────────────────────

bool TranslationManager::loadFromSD(const char* lang) {
  char path[32];
  snprintf(path, sizeof(path), "/config/lang/%s.lang", lang);

  const unsigned long loadStart = millis();
  LOG_DBG("I18N", "Loading: %s, free heap: %u", path, static_cast<unsigned>(ESP.getFreeHeap()));

  if (!Storage.exists(path)) {
    LOG_ERR("I18N", "File not found");
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("I18N", path, file)) {
    LOG_ERR("I18N", "Failed to open file");
    return false;
  }

  // Allocate valueOffsets array, initialized to 0xFFFF (no translation).
  valueOffsets = static_cast<uint16_t*>(malloc(LANG_KEY_COUNT * sizeof(uint16_t)));
  if (!valueOffsets) {
    LOG_ERR("I18N", "Failed to allocate valueOffsets");
    file.close();
    return false;
  }
  memset(valueOffsets, 0xFF, LANG_KEY_COUNT * sizeof(uint16_t));

  // Allocate initial string pool.
  pool = static_cast<char*>(malloc(INITIAL_POOL_SIZE));
  if (!pool) {
    LOG_ERR("I18N", "Failed to allocate pool");
    free(valueOffsets);
    valueOffsets = nullptr;
    file.close();
    return false;
  }
  poolSize = INITIAL_POOL_SIZE;
  poolUsed = 0;
  loadedCount = 0;

  char lineBuf[MAX_LINE_LENGTH];
  int lineNum = 0;

  while (file.available()) {
    const int bytesRead = file.fgets(lineBuf, sizeof(lineBuf));
    if (bytesRead <= 0) break;
    lineNum++;

    // Strip trailing newline/carriage return.
    int len = bytesRead;
    while (len > 0 && (lineBuf[len - 1] == '\n' || lineBuf[len - 1] == '\r')) {
      lineBuf[--len] = '\0';
    }

    // Skip empty lines and comments.
    if (len == 0 || lineBuf[0] == '#') continue;

    char* eq = strchr(lineBuf, '=');
    if (!eq) continue;

    *eq = '\0';
    const char* key = lineBuf;
    const char* value = eq + 1;

    // Trim leading/trailing spaces from key.
    while (*key == ' ') key++;
    char* keyEnd = eq - 1;
    while (keyEnd > key && *keyEnd == ' ') {
      *keyEnd = '\0';
      keyEnd--;
    }

    // Trim leading spaces from value.
    while (*value == ' ') value++;

    if (key[0] == '\0') continue;

    // Skip metadata keys.
    if (strncmp(key, "language.", 9) == 0) continue;

    // Look up the key's ID via hash table.
    const uint32_t h = fnv1a(key);
    const int16_t id = lookupId(h);
    if (id < 0) continue;  // Unknown key, skip

    // Ensure pool has enough space.
    const size_t valLen = strlen(value) + 1;
    if (poolUsed + valLen > poolSize) {
      size_t newSize = poolSize;
      while (newSize < poolUsed + valLen) {
        newSize *= 2;
      }
      char* newPool = static_cast<char*>(realloc(pool, newSize));
      if (!newPool) {
        LOG_ERR("I18N", "Pool realloc failed at line %d", lineNum);
        break;
      }
      pool = newPool;
      poolSize = newSize;
    }

    // Store value in pool and record its offset.
    memcpy(pool + poolUsed, value, valLen);
    valueOffsets[id] = static_cast<uint16_t>(poolUsed);
    poolUsed += valLen;
    loadedCount++;
  }

  file.close();

  // Shrink pool to actual usage.
  if (poolUsed > 0 && poolUsed < poolSize) {
    char* shrunk = static_cast<char*>(realloc(pool, poolUsed));
    if (shrunk) {
      pool = shrunk;
      poolSize = poolUsed;
    }
  }

  LOG_INF("I18N", "Loaded %u/%u translations in %lu ms, pool %u bytes, free heap: %u", loadedCount, LANG_KEY_COUNT,
          millis() - loadStart, static_cast<unsigned>(poolUsed), static_cast<unsigned>(ESP.getFreeHeap()));
  return loadedCount > 0;
}

// ──────────────────────────────────────────────
// Validation
// ──────────────────────────────────────────────

bool TranslationManager::isValidLangCode(const char* lang) {
  if (!lang) return false;
  const size_t len = strlen(lang);
  if (len < 2 || len > 7) return false;
  for (size_t i = 0; i < len; i++) {
    if (lang[i] < 'a' || lang[i] > 'z') return false;
  }
  return true;
}
