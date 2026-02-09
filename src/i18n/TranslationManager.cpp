#include "TranslationManager.h"

#include <HalStorage.h>
#include <HardwareSerial.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

static constexpr size_t INITIAL_POOL_SIZE = 8192;
static constexpr uint16_t INITIAL_PAIR_CAP = 256;
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
  Serial.println("[i18n] Initializing");

  freeAll();

  // For English, no translations needed — T() returns the key itself.
  bool sdLoaded = true;
  if (strcmp(lang, "en") != 0) {
    if (isValidLangCode(lang)) {
      sdLoaded = loadFromSD(lang);
      if (!sdLoaded) {
        Serial.printf("[i18n] WARNING: Failed to load '%s', using English\n", lang);
      }
    } else {
      Serial.printf("[i18n] WARNING: Invalid language code '%s'\n", lang);
      sdLoaded = false;
    }
  }

  strncpy(currentLang, sdLoaded ? lang : "en", sizeof(currentLang) - 1);
  currentLang[sizeof(currentLang) - 1] = '\0';

  Serial.printf("[i18n] Active: %s (%u strings, %u bytes)\n", currentLang, pairCount, static_cast<unsigned>(poolUsed));
  return sdLoaded;
}

const char* TranslationManager::getString(const char* key) const {
  const int idx = findKey(key);
  if (idx >= 0) {
    return pool + pairs[idx].valueOffset;
  }
  // Key not found — return the key itself (works as English fallback).
  return key;
}

size_t TranslationManager::getMemoryUsage() const {
  return poolSize + (pairCapacity * sizeof(Pair));
}

// ──────────────────────────────────────────────
// Language scanning
// ──────────────────────────────────────────────

const std::vector<TranslationManager::LangInfo>& TranslationManager::getAvailableLanguages() {
  if (languagesScanned) {
    return availableLanguages;
  }

  availableLanguages.clear();

  // English is always available (built-in fallback).
  LangInfo english{};
  strncpy(english.code, "en", sizeof(english.code));
  strncpy(english.name, "English", sizeof(english.name));
  availableLanguages.push_back(english);

  // Scan /config/lang/ directory on SD card.
  FsFile dir = Storage.open("/config/lang");
  if (dir && dir.isDirectory()) {
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) continue;

      char filename[32];
      file.getName(filename, sizeof(filename));

      // Must end in .lang
      const size_t nameLen = strlen(filename);
      if (nameLen < 6 || strcmp(filename + nameLen - 5, ".lang") != 0) continue;

      // Extract language code from filename (e.g. "cs.lang" -> "cs")
      char code[8] = {};
      const size_t codeLen = nameLen - 5;
      if (codeLen < 2 || codeLen > 7) continue;
      memcpy(code, filename, codeLen);
      code[codeLen] = '\0';

      // Skip "en" — already added.
      if (strcmp(code, "en") == 0) continue;

      if (!isValidLangCode(code)) continue;

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

      LangInfo info{};
      strncpy(info.code, code, sizeof(info.code));
      strncpy(info.name, displayName, sizeof(info.name));
      availableLanguages.push_back(info);
    }
    dir.close();
  }

  // Sort non-English entries alphabetically by display name.
  if (availableLanguages.size() > 1) {
    std::sort(availableLanguages.begin() + 1, availableLanguages.end(),
              [](const LangInfo& a, const LangInfo& b) { return strcmp(a.name, b.name) < 0; });
  }

  languagesScanned = true;
  Serial.printf("[i18n] Found %u languages\n", static_cast<unsigned>(availableLanguages.size()));
  return availableLanguages;
}

std::vector<std::string> TranslationManager::getAvailableLanguageNames() {
  const auto& langs = getAvailableLanguages();
  std::vector<std::string> names;
  names.reserve(langs.size());
  for (const auto& lang : langs) {
    names.emplace_back(lang.name);
  }
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

  free(pairs);
  pairs = nullptr;
  pairCount = 0;
  pairCapacity = 0;
}

bool TranslationManager::ensurePoolCapacity(const size_t bytes) {
  if (pool && poolUsed + bytes <= poolSize) {
    return true;
  }

  size_t newSize = poolSize == 0 ? INITIAL_POOL_SIZE : poolSize;
  while (newSize < poolUsed + bytes) {
    newSize *= 2;
  }

  char* newPool = static_cast<char*>(realloc(pool, newSize));
  if (!newPool) {
    Serial.println("[i18n] ERROR: Pool realloc failed");
    return false;
  }
  pool = newPool;
  poolSize = newSize;
  return true;
}

bool TranslationManager::ensurePairCapacity() {
  if (pairs && pairCount < pairCapacity) {
    return true;
  }

  const uint16_t newCap = pairCapacity == 0 ? INITIAL_PAIR_CAP : pairCapacity * 2;
  auto* newPairs = static_cast<Pair*>(realloc(pairs, newCap * sizeof(Pair)));
  if (!newPairs) {
    Serial.println("[i18n] ERROR: Pairs realloc failed");
    return false;
  }
  pairs = newPairs;
  pairCapacity = newCap;
  return true;
}

// ──────────────────────────────────────────────
// Key-value storage (sorted array + binary search)
// ──────────────────────────────────────────────

int TranslationManager::findKey(const char* key) const {
  if (!pairs || pairCount == 0) {
    return -1;
  }

  int lo = 0;
  int hi = pairCount - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const int cmp = strcmp(key, pool + pairs[mid].keyOffset);
    if (cmp == 0) {
      return mid;
    }
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return -1;
}

bool TranslationManager::putString(const char* key, const char* value) {
  const size_t keyLen = strlen(key) + 1;
  const size_t valLen = strlen(value) + 1;

  // Check if key already exists (update value).
  const int existingIdx = findKey(key);
  if (existingIdx >= 0) {
    if (!ensurePoolCapacity(valLen)) {
      return false;
    }
    const uint16_t newValOffset = static_cast<uint16_t>(poolUsed);
    memcpy(pool + poolUsed, value, valLen);
    poolUsed += valLen;
    pairs[existingIdx].valueOffset = newValOffset;
    return true;
  }

  // New key — append both key and value to pool.
  if (!ensurePoolCapacity(keyLen + valLen) || !ensurePairCapacity()) {
    return false;
  }

  const uint16_t keyOffset = static_cast<uint16_t>(poolUsed);
  memcpy(pool + poolUsed, key, keyLen);
  poolUsed += keyLen;

  const uint16_t valOffset = static_cast<uint16_t>(poolUsed);
  memcpy(pool + poolUsed, value, valLen);
  poolUsed += valLen;

  // Find insertion point to keep sorted order.
  int insertPos = 0;
  {
    int lo = 0;
    int hi = pairCount;
    while (lo < hi) {
      const int mid = lo + (hi - lo) / 2;
      if (strcmp(key, pool + pairs[mid].keyOffset) > 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    insertPos = lo;
  }

  if (insertPos < pairCount) {
    memmove(&pairs[insertPos + 1], &pairs[insertPos], (pairCount - insertPos) * sizeof(Pair));
  }

  pairs[insertPos] = {keyOffset, valOffset};
  pairCount++;
  return true;
}

// ──────────────────────────────────────────────
// SD card loader
// ──────────────────────────────────────────────

bool TranslationManager::loadFromSD(const char* lang) {
  char path[32];
  snprintf(path, sizeof(path), "/config/lang/%s.lang", lang);

  Serial.printf("[i18n] Loading: %s\n", path);

  if (!Storage.exists(path)) {
    Serial.println("[i18n] File not found");
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("I18N", path, file)) {
    Serial.println("[i18n] Failed to open file");
    return false;
  }

  char lineBuf[MAX_LINE_LENGTH];
  int lineNum = 0;
  int loaded = 0;

  while (file.available()) {
    const int bytesRead = file.fgets(lineBuf, sizeof(lineBuf));
    if (bytesRead <= 0) {
      break;
    }
    lineNum++;

    // Strip trailing newline/carriage return.
    int len = bytesRead;
    while (len > 0 && (lineBuf[len - 1] == '\n' || lineBuf[len - 1] == '\r')) {
      lineBuf[--len] = '\0';
    }

    // Skip empty lines and comments.
    if (len == 0 || lineBuf[0] == '#') {
      continue;
    }

    char* eq = strchr(lineBuf, '=');
    if (!eq) {
      Serial.printf("[i18n] WARNING: Invalid line %d\n", lineNum);
      continue;
    }

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

    if (key[0] == '\0') {
      continue;
    }

    // Skip metadata keys (language.name is only used during scanning).
    if (strncmp(key, "language.", 9) == 0) {
      continue;
    }

    putString(key, value);
    loaded++;
  }

  file.close();
  Serial.printf("[i18n] Parsed %d lines, loaded %d translations\n", lineNum, loaded);
  return loaded > 0;
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
