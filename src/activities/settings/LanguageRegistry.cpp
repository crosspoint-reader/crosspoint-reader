#include "LanguageRegistry.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18nKeys.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

static constexpr const char* MANIFEST_PATH = "/.crosspoint/languages/manifest.json";
static constexpr size_t MAX_MANIFEST_BYTES = 8 * 1024;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read _language_code and _language_name from the first ~20 lines of a YAML
// file.  Returns true if both fields were found.
static bool readYamlMeta(FsFile& file, char* code, size_t codeLen, char* name, size_t nameLen) {
  code[0] = '\0';
  name[0] = '\0';

  char line[128];
  size_t pos = 0;
  int linesRead = 0;

  auto readChar = [&](char& c) -> bool {
    uint8_t b;
    if (file.read(&b, 1) != 1) return false;
    c = static_cast<char>(b);
    return true;
  };

  auto readLine = [&]() -> bool {
    pos = 0;
    char c;
    while (readChar(c)) {
      if (c == '\n') break;
      if (c != '\r' && pos < sizeof(line) - 1) line[pos++] = c;
    }
    line[pos] = '\0';
    return pos > 0 || c == '\n';
  };

  auto parseValue = [](const char* line, const char* prefix, char* out, size_t outLen) -> bool {
    const size_t plen = strlen(prefix);
    if (strncmp(line, prefix, plen) != 0) return false;
    const char* p = line + plen;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n < outLen - 1) out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
  };

  while (linesRead++ < 20) {
    char c;
    // Read one line
    pos = 0;
    bool got = false;
    while (readChar(c)) {
      got = true;
      if (c == '\n') break;
      if (c != '\r' && pos < sizeof(line) - 1) line[pos++] = c;
    }
    line[pos] = '\0';
    if (!got) break;

    parseValue(line, "_language_code:", code, codeLen);
    parseValue(line, "_language_name:", name, nameLen);

    if (code[0] && name[0]) return true;
  }
  return code[0] != '\0';  // at least code found
}

// Return true if an entry with the given code already exists in the list.
static bool hasCode(const std::vector<LanguageEntry>& list, const char* code) {
  for (const auto& e : list) {
    if (strcmp(e.code, code) == 0) return true;
  }
  return false;
}

static bool isValidLanguageCode(const char* code) {
  return code && code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z' && code[2] == '\0';
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool LanguageRegistry::hasManifest() {
  FsFile f;
  if (!Storage.openFileForRead("LANG", MANIFEST_PATH, f)) return false;
  f.close();
  return true;
}

std::vector<LanguageEntry> LanguageRegistry::buildList() {
  std::vector<LanguageEntry> list;

  // 1. Core languages (always installed, in enum order).
  for (uint8_t i = 0; i < LANGUAGE_META_COUNT; i++) {
    LanguageEntry e{};
    strncpy(e.code, ALL_LANGUAGES[i].code, sizeof(e.code) - 1);
    strncpy(e.name, ALL_LANGUAGES[i].name, sizeof(e.name) - 1);
    e.isCore = true;
    e.isInstalled = true;
    list.push_back(e);
  }

  // 2. Scan SD card for installed non-core YAML files.
  {
    auto dir = Storage.open("/.crosspoint/languages");
    if (dir && dir.isDirectory()) {
      for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
        if (file.isDirectory()) {
          file.close();
          continue;
        }

        char fname[64];
        file.getName(fname, sizeof(fname));

        // Only process XX.yaml files (not manifest.json).
        const size_t flen = strlen(fname);
        if (flen < 6 || strcmp(fname + flen - 5, ".yaml") != 0) {
          file.close();
          continue;
        }

        char code[8] = {};
        char name[48] = {};
        readYamlMeta(file, code, sizeof(code), name, sizeof(name));
        file.close();

        if (!isValidLanguageCode(code) || hasCode(list, code)) continue;

        LanguageEntry e{};
        strncpy(e.code, code, sizeof(e.code) - 1);
        if (name[0])
          strncpy(e.name, name, sizeof(e.name) - 1);
        else
          strncpy(e.name, code, sizeof(e.name) - 1);  // fallback to code
        e.isCore = false;
        e.isInstalled = true;
        list.push_back(e);
      }
    }
    if (dir) dir.close();
  }

  // 3. Load manifest for available-but-not-installed languages.
  {
    FsFile file;
    if (Storage.openFileForRead("LANG", MANIFEST_PATH, file)) {
      const size_t manifestSize = static_cast<size_t>(file.size());
      if (manifestSize == 0 || manifestSize > MAX_MANIFEST_BYTES) {
        LOG_ERR("LANG_REG", "manifest.json size invalid: %zu", manifestSize);
        file.close();
        return list;
      }

      JsonDocument doc;
      const DeserializationError err = deserializeJson(doc, file);
      file.close();

      if (!err) {
        for (JsonObject lang : doc.as<JsonArray>()) {
          const char* code = lang["code"] | "";
          const char* name = lang["name"] | "";
          if (!isValidLanguageCode(code) || hasCode(list, code)) continue;

          LanguageEntry e{};
          strncpy(e.code, code, sizeof(e.code) - 1);
          strncpy(e.name, name[0] ? name : code, sizeof(e.name) - 1);
          e.isCore = false;
          e.isInstalled = false;
          list.push_back(e);
        }
      } else {
        LOG_ERR("LANG_REG", "manifest.json parse error: %s", err.c_str());
      }
    }
  }

  // 4. Sort: EN first, then alphabetical by name (case-insensitive).
  auto enIt = std::find_if(list.begin(), list.end(), [](const LanguageEntry& e) { return strcmp(e.code, "EN") == 0; });
  if (enIt != list.end() && enIt != list.begin()) {
    std::rotate(list.begin(), enIt, enIt + 1);
  }
  std::sort(list.begin() + 1, list.end(),
            [](const LanguageEntry& a, const LanguageEntry& b) { return strcasecmp(a.name, b.name) < 0; });

  return list;
}
