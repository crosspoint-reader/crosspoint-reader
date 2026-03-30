#include "DictionaryManager.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

// ---------------------------------------------------------------------------
// scan — discover .dict files in DICT_DIR, validate indexes
// ---------------------------------------------------------------------------
void DictionaryManager::scan() {
  if (scanned) return;
  dictCount = 0;
  scanned = true;

  if (!Storage.exists(DICT_DIR)) {
    LOG_INF("DICTM", "Dictionary directory not found: %s", DICT_DIR);
    return;
  }

  auto dir = Storage.open(DICT_DIR);
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("DICTM", "Failed to open dictionary directory");
    if (dir) dir.close();
    return;
  }

  char name[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    file.close();

    // Skip hidden files
    if (name[0] == '.') continue;

    // Check for .dict extension
    int nameLen = static_cast<int>(strlen(name));
    if (nameLen < 6) continue;  // minimum: "x.dict"
    if (strcasecmp(name + nameLen - 5, ".dict") != 0) continue;

    if (dictCount >= MAX_DICTIONARIES) {
      LOG_INF("DICTM", "Max dictionaries reached (%d), skipping rest", MAX_DICTIONARIES);
      break;
    }

    DictFileInfo& info = dictionaries[dictCount];
    info.enabled = false;
    info.corrupt = false;
    info.readOnly = false;

    // Strip extension for filename — skip if too long to store
    int baseLen = nameLen - 5;
    if (baseLen >= static_cast<int>(sizeof(info.filename))) {
      LOG_ERR("DICTM", "Dictionary filename too long, skipping: %s", name);
      continue;
    }
    memcpy(info.filename, name, baseLen);
    info.filename[baseLen] = '\0';

    // Title-case for display name
    titleCaseDictName(info.filename, info.displayName, sizeof(info.displayName));

    // Build full path and validate/generate index
    char fullPath[192];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", DICT_DIR, name);

    bool corrupt = false;
    bool readOnly = false;
    index.ensureIndex(fullPath, corrupt, readOnly);
    info.corrupt = corrupt;
    info.readOnly = readOnly;

    LOG_INF("DICTM", "Found dictionary: %s (corrupt=%d, readOnly=%d)", info.displayName, info.corrupt, info.readOnly);

    dictCount++;
  }
  dir.close();

  loadEnabledState();

  LOG_INF("DICTM", "Scan complete: %d dictionaries found", dictCount);
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

  int found = 0;
  char fullPath[192];

  for (int i = 0; i < dictCount && found < maxResults; ++i) {
    if (!dictionaries[i].enabled || dictionaries[i].corrupt || dictionaries[i].readOnly) continue;

    snprintf(fullPath, sizeof(fullPath), "%s/%s.dict", DICT_DIR, dictionaries[i].filename);

    DictResult& result = results[found];

    // Try exact word first
    bool matched = index.lookup(fullPath, word, result.definition, sizeof(result.definition));

    // If not found, try normalized word (only if normalization changes something)
    if (!matched) {
      char normalized[DICT_WORD_MAX];
      if (normalizeWord(word, normalized, sizeof(normalized)) && strcmp(normalized, word) != 0) {
        matched = index.lookup(fullPath, normalized, result.definition, sizeof(result.definition));
      }
    }

    if (matched) {
      // Copy dictionary display name
      strncpy(result.dictionaryName, dictionaries[i].displayName, sizeof(result.dictionaryName) - 1);
      result.dictionaryName[sizeof(result.dictionaryName) - 1] = '\0';
      found++;
      LOG_INF("DICTM", "Found '%s' in %s", word, dictionaries[i].displayName);
    }
  }

  if (found == 0) {
    LOG_INF("DICTM", "No results for '%s'", word);
  }

  return found;
}
