#include "SdCardThemeRegistry.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
constexpr int THEME_SCHEMA_VERSION = 1;
}  // namespace

const char* SdCardThemeRegistry::activeDeviceId() { return gpio.deviceIsX3() ? "x3" : "x4"; }

bool SdCardThemeRegistry::isSafeId(const char* value) {
  if (value == nullptr || value[0] == '\0') return false;
  if (strstr(value, "..") != nullptr || strchr(value, '/') != nullptr || strchr(value, '\\') != nullptr) return false;
  for (const char* p = value; *p != '\0'; ++p) {
    const char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') return false;
  }
  return true;
}

SdThemeRendererHint SdCardThemeRegistry::rendererHintFor(const char* id, const char* componentModule) {
  if (componentModule != nullptr && strcmp(componentModule, "carousel") == 0) return SdThemeRendererHint::Carousel;
  if (id != nullptr) {
    if (strcmp(id, "classic") == 0 || strcmp(id, "Classic") == 0) return SdThemeRendererHint::Lyra;
    if (strcmp(id, "lyra-3-covers") == 0 || strcmp(id, "Lyra 3 Covers") == 0 || strcmp(id, "LYRA_3_COVERS") == 0) {
      return SdThemeRendererHint::Lyra;
    }
    if (strcmp(id, "roundedraff") == 0 || strcmp(id, "RoundedRaff") == 0) return SdThemeRendererHint::Lyra;
  }
  return SdThemeRendererHint::Lyra;
}

bool SdCardThemeRegistry::parseThemeJson(const char* themeDirPath, SdCardThemeInfo& out) {
  char jsonPath[180];
  snprintf(jsonPath, sizeof(jsonPath), "%s/theme.json", themeDirPath);

  HalFile file;
  if (!Storage.openFileForRead("THREG", jsonPath, file)) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_ERR("THREG", "Theme JSON parse error in %s: %s", jsonPath, err.c_str());
    return false;
  }

  const int schema = doc["schema"] | 0;
  if (schema != THEME_SCHEMA_VERSION) {
    LOG_ERR("THREG", "Unsupported theme schema %d in %s", schema, jsonPath);
    return false;
  }

  const char* id = doc["id"] | "";
  const char* name = doc["name"] | id;
  if (!isSafeId(id) || !isSafeId(name)) {
    LOG_ERR("THREG", "Invalid theme id/name in %s", jsonPath);
    return false;
  }

  const char* deviceId = activeDeviceId();
  JsonObject deviceObj = doc["devices"][deviceId].as<JsonObject>();

  const char* inherits = deviceObj["inherits"] | doc["inherits"] | "lyra";
  const char* homeRecentsModule =
      deviceObj["components"]["homeRecents"]["module"] | doc["components"]["homeRecents"]["module"] | nullptr;

  out.id = id;
  out.name = name;
  out.path = themeDirPath;
  out.inherits = inherits;
  out.deviceId = deviceId;
  out.constraints.screenWidth = deviceObj["constraints"]["screenWidth"] | doc["constraints"]["screenWidth"] | 0;
  out.constraints.screenHeight = deviceObj["constraints"]["screenHeight"] | doc["constraints"]["screenHeight"] | 0;
  out.constraints.frontButtons = deviceObj["constraints"]["frontButtons"] | doc["constraints"]["frontButtons"] | 0;
  out.constraints.sideButtons =
      (deviceObj["constraints"]["sideButtons"] | doc["constraints"]["sideButtons"] | "");
  out.rendererHint = rendererHintFor(id, homeRecentsModule);
  return true;
}

void SdCardThemeRegistry::scanRoot(const char* rootPath, std::vector<SdCardThemeInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("THREG", "Themes directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("THREG", "Themes path is not a directory: %s", rootPath);
    return;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
    if (!isSafeId(nameBuffer)) continue;

    char themeDirPath[180];
    snprintf(themeDirPath, sizeof(themeDirPath), "%s/%s", rootPath, nameBuffer);

    SdCardThemeInfo info;
    if (!parseThemeJson(themeDirPath, info)) continue;

    bool exists = false;
    for (const auto& theme : out) {
      if (theme.id == info.id) {
        exists = true;
        break;
      }
    }
    if (exists) continue;

    LOG_DBG("THREG", "Found theme: %s (%s)", info.name.c_str(), info.path.c_str());
    out.push_back(std::move(info));
  }
}

bool SdCardThemeRegistry::discover() {
  themes_.clear();
  themes_.reserve(MAX_SD_THEMES);

  scanRoot(THEMES_DIR_HIDDEN, themes_);
  scanRoot(THEMES_DIR_VISIBLE, themes_);

  std::sort(themes_.begin(), themes_.end(),
            [](const SdCardThemeInfo& a, const SdCardThemeInfo& b) { return a.name < b.name; });

  if (static_cast<int>(themes_.size()) > MAX_SD_THEMES) {
    themes_.resize(MAX_SD_THEMES);
  }

  LOG_DBG("THREG", "Discovery complete: %d themes", static_cast<int>(themes_.size()));
  return !themes_.empty();
}

const SdCardThemeInfo* SdCardThemeRegistry::findTheme(const std::string& id) const {
  auto it = std::find_if(themes_.begin(), themes_.end(), [&](const SdCardThemeInfo& theme) {
    return theme.id == id || theme.name == id;
  });
  return it == themes_.end() ? nullptr : &*it;
}

const char* SdCardThemeRegistry::findThemeRoot(const char* themeId) {
  if (!isSafeId(themeId)) return nullptr;
  char path[180];
  snprintf(path, sizeof(path), "%s/%s", THEMES_DIR_HIDDEN, themeId);
  if (Storage.exists(path)) return THEMES_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", THEMES_DIR_VISIBLE, themeId);
  if (Storage.exists(path)) return THEMES_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardThemeRegistry::defaultWriteRoot() {
  const bool hiddenExists = Storage.exists(THEMES_DIR_HIDDEN);
  const bool visibleExists = Storage.exists(THEMES_DIR_VISIBLE);
  if (hiddenExists) return THEMES_DIR_HIDDEN;
  if (visibleExists) return THEMES_DIR_VISIBLE;
  return THEMES_DIR_HIDDEN;
}
