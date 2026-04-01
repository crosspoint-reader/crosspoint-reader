#include "DictionaryManager.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

// ---------------------------------------------------------------------------
// scan — discover StarDict .ifo files in DICT_DIR and subdirectories
// ---------------------------------------------------------------------------
void DictionaryManager::scan() {
  if (scanned) return;
  dictCount = 0;
  scanned = true;

  if (!Storage.exists(DICT_DIR)) {
    LOG_INF("DICTM", "Dictionary directory not found: %s", DICT_DIR);
    return;
  }

  // Recursively scan DICT_DIR and all subdirectories for .ifo files
  scanDirectory(DICT_DIR);

  loadEnabledState();

  LOG_INF("DICTM", "Scan complete: %d dictionaries found", dictCount);
}

// ---------------------------------------------------------------------------
// scanDirectory — find .ifo files in a single directory
// ---------------------------------------------------------------------------
void DictionaryManager::scanDirectory(const char* dirPath) {
  auto dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    bool isDir = file.isDirectory();
    file.getName(name, sizeof(name));
    file.close();

    // Skip hidden entries
    if (name[0] == '.') continue;

    // Recurse into subdirectories
    if (isDir) {
      if (dictCount < MAX_DICTIONARIES) {
        char subPath[192];
        snprintf(subPath, sizeof(subPath), "%s/%s", dirPath, name);
        scanDirectory(subPath);
      }
      continue;
    }

    // Check for .ifo extension
    int nameLen = static_cast<int>(strlen(name));
    if (nameLen < 5) continue;  // minimum: "x.ifo"
    if (strcasecmp(name + nameLen - 4, ".ifo") != 0) continue;

    if (dictCount >= MAX_DICTIONARIES) {
      LOG_INF("DICTM", "Max dictionaries reached (%d), skipping rest", MAX_DICTIONARIES);
      break;
    }

    DictFileInfo& info = dictionaries[dictCount];
    info.enabled = false;
    info.corrupt = false;
    info.readOnly = false;

    // Strip .ifo extension for base filename
    int baseLen = nameLen - 4;
    char baseFilename[64];
    if (baseLen >= static_cast<int>(sizeof(baseFilename))) {
      LOG_ERR("DICTM", "Dictionary filename too long, skipping: %s", name);
      continue;
    }
    memcpy(baseFilename, name, baseLen);
    baseFilename[baseLen] = '\0';

    // Build base path (without extension) for StarDict operations
    char basePath[192];
    snprintf(basePath, sizeof(basePath), "%s/%s", dirPath, baseFilename);

    // Store path relative to DICT_DIR in filename (e.g., "wordnet-en/wordnet")
    // so lookup() can reconstruct the full path later
    int dictDirLen = static_cast<int>(strlen(DICT_DIR));
    const char* relPath = basePath + dictDirLen + 1;  // skip DICT_DIR + '/'
    if (strlen(relPath) >= sizeof(info.filename)) {
      LOG_ERR("DICTM", "Dictionary path too long, skipping: %s", relPath);
      continue;
    }
    strncpy(info.filename, relPath, sizeof(info.filename) - 1);
    info.filename[sizeof(info.filename) - 1] = '\0';

    // Verify companion .idx and .dict files exist
    char companionPath[200];
    snprintf(companionPath, sizeof(companionPath), "%s.idx", basePath);
    if (!Storage.exists(companionPath)) {
      LOG_ERR("DICTM", "Missing .idx for %s, skipping", info.filename);
      continue;
    }
    snprintf(companionPath, sizeof(companionPath), "%s.dict", basePath);
    if (!Storage.exists(companionPath)) {
      LOG_ERR("DICTM", "Missing .dict for %s, skipping", info.filename);
      continue;
    }

    // Parse .ifo for display name (bookname), fall back to title-cased filename
    char ifoPath[200];
    snprintf(ifoPath, sizeof(ifoPath), "%s.ifo", basePath);
    char bookname[64] = {};
    if (StarDictIndex::parseIfo(ifoPath, bookname, sizeof(bookname), nullptr, nullptr, nullptr, 0) &&
        bookname[0] != '\0') {
      strncpy(info.displayName, bookname, sizeof(info.displayName) - 1);
      info.displayName[sizeof(info.displayName) - 1] = '\0';
    } else {
      titleCaseDictName(baseFilename, info.displayName, sizeof(info.displayName));
    }

    // Validate/generate secondary index (.idx.cp)
    bool corrupt = false;
    bool readOnly = false;
    index.ensureIndex(basePath, corrupt, readOnly);
    info.corrupt = corrupt;
    info.readOnly = readOnly;

    LOG_INF("DICTM", "Found dictionary: %s (corrupt=%d, readOnly=%d)", info.displayName, info.corrupt,
            info.readOnly);

    dictCount++;
  }
  dir.close();
}

// ---------------------------------------------------------------------------
// loadEnabledState — read enabled.json and match filenames to dictionaries
// ---------------------------------------------------------------------------
void DictionaryManager::loadEnabledState() {
  // Default: disable all first
  for (int i = 0; i < dictCount; ++i) {
    dictionaries[i].enabled = false;
  }

  if (!Storage.exists(ENABLED_FILE)) {
    // No enabled.json — enable "english" by default if it exists and is not corrupt
    for (int i = 0; i < dictCount; ++i) {
      if (!dictionaries[i].corrupt && strcasecmp(dictionaries[i].filename, "english") == 0) {
        dictionaries[i].enabled = true;
        LOG_INF("DICTM", "Default-enabled: %s", dictionaries[i].displayName);
        break;
      }
    }
    return;
  }

  // Buffer sized for MAX_DICTIONARIES × max filename (64) + JSON overhead
  char buf[1536];
  size_t bytesRead = Storage.readFileToBuffer(ENABLED_FILE, buf, sizeof(buf));
  if (bytesRead == 0) {
    LOG_ERR("DICTM", "Failed to read enabled.json");
    return;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, buf);
  if (error) {
    LOG_ERR("DICTM", "JSON parse error: %s", error.c_str());
    return;
  }

  JsonArray arr = doc["enabled"].as<JsonArray>();
  if (arr.isNull()) {
    LOG_ERR("DICTM", "enabled.json missing 'enabled' array");
    return;
  }

  for (JsonVariant v : arr) {
    const char* enabledName = v.as<const char*>();
    if (!enabledName) continue;

    for (int i = 0; i < dictCount; ++i) {
      if (!dictionaries[i].corrupt && strcasecmp(dictionaries[i].filename, enabledName) == 0) {
        dictionaries[i].enabled = true;
        LOG_INF("DICTM", "Enabled: %s", dictionaries[i].displayName);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// saveEnabledState — write enabled.json with list of enabled dictionary names
// ---------------------------------------------------------------------------
void DictionaryManager::saveEnabledState() {
  JsonDocument doc;
  JsonArray arr = doc["enabled"].to<JsonArray>();

  for (int i = 0; i < dictCount; ++i) {
    if (dictionaries[i].enabled) {
      arr.add(dictionaries[i].filename);
    }
  }

  // Buffer sized for MAX_DICTIONARIES × max filename (64) + JSON overhead
  char buf[1536];
  size_t written = serializeJson(doc, buf, sizeof(buf));
  if (written == 0 || written >= sizeof(buf)) {
    LOG_ERR("DICTM", "Failed to serialize enabled.json");
    return;
  }

  Storage.ensureDirectoryExists(DICT_DIR);
  if (!Storage.writeFile(ENABLED_FILE, String(buf))) {
    LOG_ERR("DICTM", "Failed to write enabled.json");
  } else {
    LOG_INF("DICTM", "Saved enabled state (%d bytes)", static_cast<int>(written));
  }
}

// ---------------------------------------------------------------------------
// setEnabled — toggle a dictionary on/off
// ---------------------------------------------------------------------------
void DictionaryManager::setEnabled(int idx, bool enabled) {
  if (idx < 0 || idx >= dictCount) return;
  dictionaries[idx].enabled = enabled;
}

// ---------------------------------------------------------------------------
// hasEnabledDictionaries — check if any non-corrupt dictionary is enabled
// ---------------------------------------------------------------------------
bool DictionaryManager::hasEnabledDictionaries() const {
  for (int i = 0; i < dictCount; ++i) {
    if (dictionaries[i].enabled && !dictionaries[i].corrupt && !dictionaries[i].readOnly) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// normalizeWord — strip punctuation, lowercase
// ---------------------------------------------------------------------------
// Check if a byte position starts a UTF-8 curly quote sequence.
// Returns the number of bytes consumed (3 for curly quotes, 0 if not a match).
static int isCurlyQuote(const char* p) {
  auto b = [](const char* s, int i) { return static_cast<unsigned char>(s[i]); };
  // UTF-8 sequences for U+2018..U+201D (curly single/double quotes):
  //   \xE2\x80\x98  '   U+2018
  //   \xE2\x80\x99  '   U+2019
  //   \xE2\x80\x9C  "   U+201C
  //   \xE2\x80\x9D  "   U+201D
  if (b(p, 0) == 0xE2 && b(p, 1) == 0x80) {
    unsigned char c = b(p, 2);
    if (c == 0x98 || c == 0x99 || c == 0x9C || c == 0x9D) return 3;
  }
  return 0;
}

bool DictionaryManager::normalizeWord(const char* word, char* outBuf, int outSize) {
  if (outSize <= 0) return false;

  int len = static_cast<int>(strlen(word));
  int start = 0;
  int end = len - 1;

  // Strip leading punctuation: ASCII quotes/brackets + curly quotes
  static constexpr const char* leadingStrip = "\"'([{";
  while (start <= end) {
    if (strchr(leadingStrip, word[start]) != nullptr) {
      start++;
    } else if (start + 2 <= end) {
      int skip = isCurlyQuote(word + start);
      if (skip > 0) {
        start += skip;
      } else {
        break;
      }
    } else {
      break;
    }
  }

  // Strip trailing punctuation: ASCII quotes/brackets/punct + curly quotes
  static constexpr const char* trailingStrip = "\"')]}.,:;!?";
  while (end >= start) {
    if (strchr(trailingStrip, word[end]) != nullptr) {
      end--;
    } else if (end >= start + 2) {
      int skip = isCurlyQuote(word + end - 2);
      if (skip > 0) {
        end -= skip;
      } else {
        break;
      }
    } else {
      break;
    }
  }

  int resultLen = end - start + 1;
  if (resultLen <= 0) return false;
  if (resultLen >= outSize) resultLen = outSize - 1;

  // Lowercase via tolower (cast through unsigned char for safety)
  for (int i = 0; i < resultLen; ++i) {
    outBuf[i] = static_cast<char>(tolower(static_cast<unsigned char>(word[start + i])));
  }
  outBuf[resultLen] = '\0';

  return true;
}

// ---------------------------------------------------------------------------
// lookup — search all enabled dictionaries for a word
// ---------------------------------------------------------------------------
int DictionaryManager::lookup(const char* word, DictResult* results, int maxResults) {
  if (!scanned) scan();
  if (!word || word[0] == '\0' || maxResults <= 0) return 0;

  const unsigned long startMs = millis();
  int found = 0;
  char basePath[192];
  const char* matchType = nullptr;

  for (int i = 0; i < dictCount && found < maxResults; ++i) {
    if (!dictionaries[i].enabled || dictionaries[i].corrupt || dictionaries[i].readOnly) continue;

    snprintf(basePath, sizeof(basePath), "%s/%s", DICT_DIR, dictionaries[i].filename);

    DictResult& result = results[found];

    // Try exact word first
    bool matched = index.lookup(basePath, word, result.definition, sizeof(result.definition));
    if (matched && !matchType) matchType = "direct match";

    // If not found, try normalized word (only if normalization changes something)
    if (!matched) {
      char normalized[DICT_WORD_MAX];
      if (normalizeWord(word, normalized, sizeof(normalized)) && strcmp(normalized, word) != 0) {
        matched = index.lookup(basePath, normalized, result.definition, sizeof(result.definition));
        if (matched && !matchType) matchType = "normalized";
      }
    }

    if (matched) {
      strncpy(result.dictionaryName, dictionaries[i].displayName, sizeof(result.dictionaryName) - 1);
      result.dictionaryName[sizeof(result.dictionaryName) - 1] = '\0';
      found++;
    }
  }

  const unsigned long elapsedMs = millis() - startMs;
  if (found > 0) {
    LOG_INF("DICT", "Lookup '%s' -> %s in %lu ms", word, matchType, elapsedMs);
  } else {
    LOG_INF("DICT", "Lookup '%s' -> not found in %lu ms", word, elapsedMs);
  }

  return found;
}
