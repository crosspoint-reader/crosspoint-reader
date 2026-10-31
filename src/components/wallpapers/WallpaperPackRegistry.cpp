#include "WallpaperPackRegistry.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "util/WallpaperPackages.h"

namespace {
constexpr char ROOT[] = "/.crosspoint/wallpapers";
constexpr char SUFFIX[] = ".crosspoint-wallpaper";
constexpr size_t MAX_PACKS = 8;
constexpr size_t MAX_IMAGES = 32;
constexpr size_t MAX_FILES = MAX_IMAGES + 3;

bool safeId(const char* text) {
  if (!text || !*text) return false;
  size_t len = 0;
  for (const char* p = text; *p; ++p) {
    if (++len > 32 || !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || (*p == '-' && p != text))) return false;
  }
  return true;
}

bool imagePath(const char* name) {
  if (!name || strncmp(name, "wallpapers/", 11) != 0) return false;
  const char* part = name + 11;
  const size_t length = strlen(part);
  if (length < 5 || length > 80 || strcmp(part + length - 4, ".bmp") != 0) return false;
  if (*part == '.') return false;
  for (const char* p = part; *p; ++p) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
          c == '_'))
      return false;
  }
  return true;
}

bool declared(JsonArrayConst files, const char* path) {
  for (JsonVariantConst value : files) {
    const char* name = value.as<const char*>();
    if (name && strcmp(name, path) == 0) return true;
  }
  return false;
}

bool validManifest(const char* id, const std::string& base, JsonDocument& doc, bool checkImageHeaders) {
  std::string contents;
  if (!Storage.readFileToString("WPK", base + "/manifest.json", wallpaperpackages::MAX_MANIFEST_BYTES, contents)) return false;
  if (deserializeJson(doc, contents, DeserializationOption::NestingLimit(5)) != DeserializationError::Ok) return false;
  if (strcmp(doc["kind"] | "", "wallpaper-pack") || strcmp(doc["name"] | "", id)) return false;
  const char* title = doc["title"] | "";
  const char* version = doc["version"] | "";
  if (!*title || strlen(title) > 60 || !*version || strlen(version) > 24) return false;
  JsonArrayConst images = doc["images"].as<JsonArrayConst>();
  JsonArrayConst files = doc["files"].as<JsonArrayConst>();
  // Keep legacy installed packs working; new package publications supply both
  // fields but old releases have neither.
  const char* description = doc["description"] | "";
  if (*description && strlen(description) > 180) return false;
  if (images.isNull() || files.isNull() || images.size() == 0 || images.size() > MAX_IMAGES ||
      (files.size() != images.size() + 2 && files.size() != images.size() + 3) ||
       files.size() > MAX_FILES || !declared(files, "manifest.json") ||
      !declared(files, "README.md") ||
       (files.size() == images.size() + 3 && !declared(files, "DETAILS.md")))
    return false;
  // Enforce a complete, explicit file list. No executable files, traversal,
  // external paths, nested collections, or arbitrary SD-file references.
  for (size_t i = 0; i < files.size(); ++i) {
    const char* path = files[i].as<const char*>();
    if (!path) return false;
    if (!imagePath(path) && strcmp(path, "manifest.json") && strcmp(path, "README.md") &&
        strcmp(path, "DETAILS.md")) return false;
    for (size_t j = 0; j < i; ++j) {
      const char* prior = files[j].as<const char*>();
      if (prior && strcmp(path, prior) == 0) return false;
    }
  }
  for (size_t i = 0; i < images.size(); ++i) {
    const char* path = images[i].as<const char*>();
    if (!imagePath(path) || !declared(files, path)) return false;
    for (size_t j = 0; j < i; ++j) {
      const char* prior = images[j].as<const char*>();
      if (prior && strcmp(path, prior) == 0) return false;
    }
    if (!checkImageHeaders) continue;
    HalFile image;
    if (!Storage.openFileForRead("WPK", base + "/" + path, image)) return false;
    const auto size = image.fileSize64();
    if (size < 54 || size > 8u * 1024u * 1024u) return false;
    Bitmap bitmap(image);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  }
  return true;
}

std::string packRoot(const char* id) { return std::string(ROOT) + "/" + id + SUFFIX; }
}  // namespace

WallpaperPackRegistry& WallpaperPackRegistry::getInstance() {
  static WallpaperPackRegistry registry;
  return registry;
}

void WallpaperPackRegistry::scan() {
  installed.clear();
  if (!Storage.ready()) return;
  // If power failed between renaming the old collection to .backup and
  // installing the new one, restore the old collection before discovery.
  // Collect names first: renaming while iterating an open SD directory is unsafe.
  std::vector<std::string> backups;
  backups.reserve(MAX_PACKS);
  {
    auto root = Storage.open(ROOT);
    if (!root || !root.isDirectory()) return;
    constexpr char BACKUP_SUFFIX[] = ".crosspoint-wallpaper.backup";
    for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) continue;
      char name[96] = {};
      entry.getName(name, sizeof(name));
      const size_t len = strlen(name);
      const size_t suffixLen = sizeof(BACKUP_SUFFIX) - 1;
      if (len <= suffixLen || strcmp(name + len - suffixLen, BACKUP_SUFFIX) != 0) continue;
      const std::string id(name, len - suffixLen);
      if (!safeId(id.c_str()) || Storage.exists(packRoot(id.c_str()).c_str())) continue;
      if (backups.size() < MAX_PACKS) backups.push_back(id);
    }
  }
  for (const auto& id : backups) {
    const std::string active = packRoot(id.c_str());
    if (!Storage.rename((active + ".backup").c_str(), active.c_str())) {
      LOG_ERR("WPK", "Failed to recover interrupted wallpaper installation: %s", id.c_str());
    } else {
      LOG_INF("WPK", "Recovered wallpaper pack from backup: %s", id.c_str());
    }
  }
  // Reopen the directory after recovery so restored packs are discoverable.
  auto directory = Storage.open(ROOT);
  if (!directory || !directory.isDirectory()) return;
  installed.reserve(MAX_PACKS);
  for (auto entry = directory.openNextFile(); entry && installed.size() < MAX_PACKS; entry = directory.openNextFile()) {
    if (!entry.isDirectory()) continue;
    char folder[80] = {};
    entry.getName(folder, sizeof(folder));
    const size_t len = strlen(folder);
    const size_t tail = sizeof(SUFFIX) - 1;
    if (len <= tail || strcmp(folder + len - tail, SUFFIX)) continue;
    const std::string id(folder, len - tail);
    if (!safeId(id.c_str())) continue;
    JsonDocument doc;
    if (!validManifest(id.c_str(), packRoot(id.c_str()), doc, true)) {
      LOG_ERR("WPK", "Skipped invalid wallpaper collection: %s", folder);
      continue;
    }
    installed.push_back({id, doc["title"].as<std::string>(), doc["description"] | "", doc["author"] | "",
                         doc["version"].as<std::string>()});
  }
  LOG_INF("WPK", "Found %u valid wallpaper packs", static_cast<unsigned>(installed.size()));
}

bool WallpaperPackRegistry::reconcileSelection() {
  if (!Storage.ready()) return false;
  if (!SETTINGS.wallpaperPackId[0]) return false;
  for (const auto& pack : installed)
    if (pack.id == SETTINGS.wallpaperPackId) return false;
  LOG_INF("WPK", "Selected wallpaper pack %s is unavailable; switching to All Wallpapers", SETTINGS.wallpaperPackId);
  SETTINGS.wallpaperPackId[0] = '\0';
  SETTINGS.saveToFile();
  return true;
}

bool WallpaperPackRegistry::remove(const std::string& id) {
  if (!safeId(id.c_str())) return false;
  bool found = false;
  for (const auto& pack : installed) {
    if (pack.id == id) {
      found = true;
      break;
    }
  }
  if (!found) return false;
  const std::string active = packRoot(id.c_str());
  // Prevent an orphaned backup from restoring an explicitly uninstalled pack.
  const std::string backup = active + ".backup";
  if (Storage.exists(backup.c_str()) && !Storage.removeDir(backup.c_str())) return false;
  if (!Storage.removeDir(active.c_str())) return false;
  scan();
  reconcileSelection();
  return true;
}

bool WallpaperPackRegistry::selectImage(const char* id, std::string& path) const {
  if (!safeId(id)) return false;
  const std::string root = packRoot(id);
  JsonDocument doc;
  if (!validManifest(id, root, doc, false)) {
    LOG_ERR("WPK", "Wallpaper pack unavailable or invalid: %s", id);
    return false;
  }
  JsonArrayConst images = doc["images"].as<JsonArrayConst>();
  const auto start = static_cast<size_t>(random(images.size()));
  for (size_t i = 0; i < images.size(); ++i) {
    const char* rel = images[(start + i) % images.size()].as<const char*>();
    if (!rel) continue;
    path = root + "/" + rel;
    return true;
  }
  return false;
}
