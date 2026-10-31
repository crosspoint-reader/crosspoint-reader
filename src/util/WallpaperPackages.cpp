#include "util/WallpaperPackages.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Bitmap.h>
#include <HalStorage.h>
#include <JsonListParser.h>
#include <Logging.h>
#include <ZipFile.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include "components/wallpapers/WallpaperPackRegistry.h"

namespace wallpaperpackages {
namespace {
constexpr size_t MAX_FILES = 80;
constexpr size_t MAX_EXPANDED_BYTES = 64u * 1024u * 1024u;
constexpr size_t MAX_FILE_BYTES = 8u * 1024u * 1024u;
constexpr size_t MAX_VARIANTS = 8;
constexpr size_t HASH_CHUNK = 1024;

bool safeId(const std::string& value) {
  if (value.empty() || value.size() > 32 ||
      !((value[0] >= 'a' && value[0] <= 'z') || (value[0] >= '0' && value[0] <= '9')))
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](const char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'; });
}

bool safeRelative(const std::string& path) {
  if (path.empty() || path.size() > 150 || path[0] == '/' || path[0] == '.' || path.find("..") != std::string::npos ||
      path.find("//") != std::string::npos || path.find('\\') != std::string::npos ||
      path.find(':') != std::string::npos || path.back() == '/')
    return false;
  return std::all_of(path.begin(), path.end(), [](const char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
           ch == '.' || ch == '/';
  });
}

bool validHash(const std::string& hash) {
  if (hash.size() != 64) return false;
  return std::all_of(hash.begin(), hash.end(),
                     [](const char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

bool validRelease(const Release& release) {
  if (!safeId(release.name) || !safeId(release.variant) || !validHash(release.sha256) || release.version.empty() ||
      release.version.size() > 24 || release.version.find_first_not_of("0123456789.") != std::string::npos)
    return false;
  const std::string suffix = "/" + release.variant + "-" + release.version + ".crosspoint-wallpaper.zip";
  return release.url.rfind("https://", 0) == 0 && release.url.size() >= suffix.size() &&
         release.url.compare(release.url.size() - suffix.size(), suffix.size(), suffix) == 0;
}
struct SelectedRow {
  const std::string& name;
  JsonListParser::Row row;
  bool matched = false;
};

void captureRow(void* context, JsonListParser::Row& row) {
  auto& target = *static_cast<SelectedRow*>(context);
  if (row.field[JsonListParser::F_TITLE] != target.name) return;
  target.row = std::move(row);
  target.matched = true;
}

bool readRow(const std::string& path, const std::string& collection, const std::string& itemsPath,
             const std::string* const (&fields)[JsonListParser::F_COUNT], const std::string& arrayPath,
             JsonListParser::Row& result) {
  HalFile file;
  if (!Storage.openFileForRead("EXT", path, file)) return false;
  SelectedRow selected{collection, {}, false};
  auto parser = std::unique_ptr<JsonListParser>(
      new (std::nothrow) JsonListParser(itemsPath, fields, arrayPath, captureRow, &selected));
  if (!parser) return false;
  const bool ok = parser->parse(
      [](void* context, char* buffer, size_t length) { return static_cast<HalFile*>(context)->read(buffer, length); },
      &file);
  if (!ok || !selected.matched) return false;
  result = std::move(selected.row);
  return true;
}

bool digestMatches(const std::string& path, const std::string& expected) {
  HalFile file;
  if (!Storage.openFileForRead("EXT", path, file)) return false;
  if (file.fileSize64() == 0 || file.fileSize64() > MAX_ARCHIVE_BYTES) return false;
  uint8_t buffer[HASH_CHUNK];
  uint8_t hash[32] = {};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  bool ok = true;
  for (int n; (n = file.read(buffer, sizeof(buffer))) > 0;) {
    if (mbedtls_sha256_update(&ctx, buffer, static_cast<size_t>(n)) != 0) {
      ok = false;
      break;
    }
  }
  if (ok) ok = mbedtls_sha256_finish(&ctx, hash) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;
  char actual[65] = {};
  for (size_t i = 0; i < sizeof(hash); ++i) snprintf(actual + 2 * i, 3, "%02x", static_cast<unsigned>(hash[i]));
  return expected == actual;
}
std::string destination(const Release& release) {
  return "/.crosspoint/wallpapers/" + release.name + ".crosspoint-wallpaper";
}
bool hasRoomForPackage(const Release& release) {
  const std::string dest = destination(release);
  if (Storage.exists(dest.c_str())) return true;
  auto& packs = WallpaperPackRegistry::getInstance();
  packs.scan();
  return packs.choices().size() < 8;
}
bool readManifest(ZipFile& archive, const Release& release, JsonDocument& manifest, std::vector<std::string>& files) {
  const std::string top = release.name + ".crosspoint-wallpaper/";
  const std::string entry = top + "manifest.json";
  size_t len = 0;
  if (!archive.getInflatedFileSize(entry.c_str(), &len) || len < 8 || len > MAX_MANIFEST_BYTES) return false;
  std::unique_ptr<uint8_t, decltype(&free)> text(archive.readFileToMemory(entry.c_str()), &free);
  if (!text || deserializeJson(manifest, reinterpret_cast<const char*>(text.get()), len,
                               DeserializationOption::NestingLimit(5)) != DeserializationError::Ok)
    return false;
  const char* kind = "wallpaper-pack";
  if (strcmp(manifest["kind"] | "", kind) != 0 || strcmp(manifest["name"] | "", release.name.c_str()) != 0 ||
      strcmp(manifest["version"] | "", release.version.c_str()) != 0)
    return false;
  const char* title = manifest["title"] | "";
  const char* description = manifest["description"] | "";
  const char* author = manifest["author"] | "";
  if (!*title || strlen(title) > 60 || strlen(description) > 180 || strlen(author) > 120) return false;
  const auto resolution = manifest["target_resolution"];
  if (resolution["width"].as<int>() != release.width || resolution["height"].as<int>() != release.height) return false;
  JsonArrayConst declared = manifest["files"].as<JsonArrayConst>();
  if (declared.isNull() || declared.size() < 2 || declared.size() > MAX_FILES) return false;
  bool readme = false, manifestFound = false, detailsFound = false;
  files.reserve(declared.size());
  for (JsonVariantConst element : declared) {
    const char* path = element.as<const char*>();
    if (!path || !safeRelative(path)) return false;
    if (std::find(files.begin(), files.end(), path) != files.end()) return false;
    files.emplace_back(path);
    if (strcmp(path, "README.md") == 0) readme = true;
    if (strcmp(path, "manifest.json") == 0) manifestFound = true;
    if (strcmp(path, "DETAILS.md") == 0) detailsFound = true;
  }
  if (!readme || !manifestFound || !detailsFound) {
    LOG_ERR("EXT", "Package missing required metadata: manifest.json, README.md, DETAILS.md");
    return false;
  }
  return true;
}
bool clearSleepBitmapCaches() {
  // Cached converted sleep BMPs are keyed by path, size, and FAT modification
  // time. FAT timestamps can remain unchanged across same-size pack updates.
  constexpr char CACHE_ROOT[] = "/.crosspoint";
  std::vector<std::string> cached;
  {
    auto root = Storage.open(CACHE_ROOT);
    if (!root || !root.isDirectory()) return false;
    for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (entry.isDirectory()) continue;
      char name[64] = {};
      if (entry.getName(name, sizeof(name)) == 0) continue;
      const size_t len = strlen(name);
      if (len <= 10 || strncmp(name, "sleep_", 6) != 0 || strcmp(name + len - 4, ".bmp") != 0) continue;
      if (!std::all_of(name + 6, name + len - 4, [](char c) { return c >= '0' && c <= '9'; })) continue;
      cached.emplace_back(std::string(CACHE_ROOT) + "/" + name);
    }
  }
  return std::all_of(cached.begin(), cached.end(), [](const std::string& path) {
    if (Storage.remove(path.c_str())) return true;
    LOG_ERR("WPK", "Unable to invalidate sleep bitmap cache: %s", path.c_str());
    return false;
  });
}
bool prepareExisting(const std::string& dest, const std::string& backup) {
  // Recover an interrupted directory swap before doing anything destructive.
  if (!Storage.exists(backup.c_str())) return true;
  if (!Storage.exists(dest.c_str())) return Storage.rename(backup.c_str(), dest.c_str());
  return Storage.removeDir(backup.c_str());
}
}  // namespace

bool selectRelease(const std::string& path, const std::string& collection, const std::string& itemsPath,
                   const uint16_t width, const uint16_t height, Release& release) {
  if (!safeId(collection)) return false;
  const std::string name = "name";
  const std::string empty;
  Release candidate;
  unsigned matches = 0;
  for (size_t i = 0; i < MAX_VARIANTS; ++i) {
    const std::string prefix = "variants." + std::to_string(i) + ".";
    const std::string namePath = prefix + "name";
    const std::string assetPath = prefix + "asset_url";
    const std::string hashPath = prefix + "sha256";
    const std::string widthPath = prefix + "target_resolution.width";
    const std::string heightPath = prefix + "target_resolution.height";
    const std::string requiresPath;
    const std::string* const fields[JsonListParser::F_COUNT] = {&name,     &namePath,  &assetPath,
                                                                &hashPath, &widthPath, &heightPath};
    JsonListParser::Row row;
    if (!readRow(path, collection, itemsPath, fields, requiresPath, row)) return false;
    if (row.field[JsonListParser::F_AUTHOR].empty()) break;
    {
      const std::string& w = row.field[JsonListParser::F_VERSION];
      const std::string& h = row.field[JsonListParser::F_BASE];
      if (w.empty() || h.empty() || w.find_first_not_of("0123456789") != std::string::npos ||
          h.find_first_not_of("0123456789") != std::string::npos)
        return false;
      if (w.size() > 5 || h.size() > 5) return false;
      if (strtoul(w.c_str(), nullptr, 10) != width || strtoul(h.c_str(), nullptr, 10) != height) continue;
    }
    if (++matches > 1) return false;
    candidate.name = row.field[JsonListParser::F_AUTHOR];
    candidate.variant = row.field[JsonListParser::F_AUTHOR];
    if (candidate.variant.rfind(collection + "-", 0) != 0) return false;
    candidate.width = width;
    candidate.height = height;
    candidate.url = row.field[JsonListParser::F_ID];
    candidate.sha256 = row.field[JsonListParser::F_URL];
  }
  if (matches != 1) return false;
  // Read release version from the same indexed collection, not from the ZIP name.
  const std::string versionPath = "version";
  const std::string* const versionFields[JsonListParser::F_COUNT] = {&name,  &versionPath, &empty,
                                                                     &empty, &empty,       &empty};
  JsonListParser::Row versionRow;
  if (!readRow(path, collection, itemsPath, versionFields, empty, versionRow)) return false;
  candidate.version = versionRow.field[JsonListParser::F_AUTHOR];
  if (!validRelease(candidate)) return false;
  release = std::move(candidate);
  return true;
}
bool installVerifiedZip(const std::string& archivePath, const Release& release) {
  const unsigned long verifyStarted = millis();
  if (!validRelease(release) || !digestMatches(archivePath, release.sha256)) {
    LOG_ERR("EXT", "Package SHA-256 or release metadata invalid: %s", release.name.c_str());
    return false;
  }
  LOG_INF("EXT", "Verified SHA-256 %s in %lu ms", release.name.c_str(), millis() - verifyStarted);
  ZipFile zip(archivePath);
  JsonDocument manifest;
  std::vector<std::string> files;
  if (!readManifest(zip, release, manifest, files)) return false;
  const std::string top = release.name + ".crosspoint-wallpaper/";
  {
    const JsonArrayConst images = manifest["images"].as<JsonArrayConst>();
    const auto resolution = manifest["target_resolution"];
    if (images.isNull() || images.size() == 0 || images.size() > 32 || resolution["width"].as<int>() != release.width ||
        resolution["height"].as<int>() != release.height)
      return false;
    if (files.size() != images.size() + 3) {
      LOG_ERR("EXT", "Wallpaper ZIP must contain exactly 3 metadata files and %u BMP images",
              static_cast<unsigned>(images.size()));
      return false;
    }
    std::vector<std::string_view> imagePaths;
    imagePaths.reserve(images.size());
    for (JsonVariantConst value : images) {
      const char* image = value.as<const char*>();
      if (!image || strncmp(image, "wallpapers/", 11) != 0 || strlen(image) < 16 || strlen(image) > 91 ||
          image[11] == '.' || strchr(image + 11, '/') != nullptr || strcmp(image + strlen(image) - 4, ".bmp") != 0 ||
          std::find(files.begin(), files.end(), image) == files.end() ||
          std::find(imagePaths.begin(), imagePaths.end(), image) != imagePaths.end())
        return false;
      imagePaths.emplace_back(image);
    }
    for (const auto& declared : files) {
      if (declared == "manifest.json" || declared == "README.md" || declared == "DETAILS.md") continue;
      if (std::find(imagePaths.begin(), imagePaths.end(), declared) == imagePaths.end()) return false;
    }
  }
  // Keep the archive handle open only during central-directory checks.
  // ZipFile supports cursor reuse for consecutive size lookups; closing
  // between files forces a fresh ZIP scan for each member.
  const unsigned long metadataStarted = millis();
  if (!zip.open()) return false;
  size_t seen = 0;
  std::vector<std::string> observedPaths;  // Installer-local, never retained by ThemeScene.
  bool validEntries = true;
  const bool scanned = zip.enumerateFilePaths([&](std::string_view entry) {
    ++seen;
    if (entry.size() < top.size() || entry.compare(0, top.size(), top) != 0) {
      validEntries = false;
      return;
    }
    const std::string relative(entry.substr(top.size()));
    if (!safeRelative(relative) ||
        std::find(observedPaths.begin(), observedPaths.end(), relative) != observedPaths.end()) {
      validEntries = false;
      return;
    }
    observedPaths.push_back(relative);
    if (std::find(files.begin(), files.end(), relative) != files.end()) {
      return;
    }
    validEntries = false;
  });
  if (!scanned || !validEntries || seen != files.size()) {
    zip.close();
    return false;
  }
  size_t expanded = 0;
  for (const auto& path : files) {
    size_t bytes = 0;
    if (!zip.getInflatedFileSize((top + path).c_str(), &bytes) || bytes > MAX_FILE_BYTES ||
        expanded > MAX_EXPANDED_BYTES - bytes) {
      zip.close();
      return false;
    }
    expanded += bytes;
  }
  // Explicitly release the ZIP handle before any inflation. The zip library
  // discourages holding it open while allocating decompression buffers.
  zip.close();
  LOG_INF("EXT", "ZIP metadata verification %s: %lu ms", release.name.c_str(), millis() - metadataStarted);

  const std::string dest = destination(release);
  const std::string temp = dest + ".installing";
  const std::string backup = dest + ".backup";
  const size_t cut = dest.find_last_of('/');
  const std::string root = dest.substr(0, cut);
  if (!Storage.ensureDirectoryExists(root.c_str()) || !prepareExisting(dest, backup) || !hasRoomForPackage(release))
    return false;
  if (Storage.exists(temp.c_str()) && !Storage.removeDir(temp.c_str())) return false;
  if (!Storage.mkdir(temp.c_str())) return false;
  // Use larger SD/inflate buffers where heap permits, but retain the
  // previous conservative setting for low-memory X3 installations.
  const size_t chunkSize = ESP.getFreeHeap() >= 90000 && ESP.getMaxAllocHeap() >= 45000 ? 4096 : 1024;
  LOG_INF("EXT", "Extracting %s: %u files, %u expanded bytes, chunk=%u", release.name.c_str(),
          static_cast<unsigned>(files.size()), static_cast<unsigned>(expanded), static_cast<unsigned>(chunkSize));
  const unsigned long extractStarted = millis();
  bool allWritten = true;
  for (const auto& relative : files) {
    const std::string full = temp + "/" + relative;
    const size_t slash = full.find_last_of('/');
    if (!Storage.ensureDirectoryExists(full.substr(0, slash).c_str())) {
      allWritten = false;
      break;
    }
    HalFile out;
    if (!Storage.openFileForWrite("EXT", full, out) ||
        !zip.readFileToStream((top + relative).c_str(), out, chunkSize)) {
      allWritten = false;
      break;
    }
    out.flush();
    if (!out.close()) {
      allWritten = false;
      break;
    }
    if (relative.rfind("wallpapers/", 0) == 0) {
      HalFile image;
      if (!Storage.openFileForRead("EXT", full, image)) {
        allWritten = false;
        break;
      }
      Bitmap bitmap(image);
      if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() != release.width ||
          bitmap.getHeight() != release.height) {
        allWritten = false;
        break;
      }
      // Bitmap::parseHeaders computes DWORD-aligned row bytes but does not
      // verify that the file includes every pixel row. Reject truncated BMPs
      // before promoting the staged package to its installed directory.
      bool offsetValid = image.seek(10);
      uint32_t pixelOffset = 0;
      for (int byte = 0; offsetValid && byte < 4; ++byte) {
        const int value = image.read();
        if (value < 0)
          offsetValid = false;
        else
          pixelOffset |= static_cast<uint32_t>(value) << (8 * byte);
      }
      const uint64_t minimumSize =
          static_cast<uint64_t>(pixelOffset) + static_cast<uint64_t>(bitmap.getRowBytes()) * bitmap.getHeight();
      if (!offsetValid || pixelOffset < 54 || image.fileSize64() < minimumSize) {
        allWritten = false;
        break;
      }
    }
  }
  LOG_INF("EXT", "ZIP extraction %s: %lu ms (%s)", release.name.c_str(), millis() - extractStarted,
          allWritten ? "ok" : "failed");
  if (!allWritten) {
    Storage.removeDir(temp.c_str());
    return false;
  }

  // Invalidate converted sleep images before replacing the pack: FAT
  // timestamps may not distinguish an updated image with the same size.
  if (!clearSleepBitmapCaches()) {
    Storage.removeDir(temp.c_str());
    return false;
  }

  bool hadPrior = Storage.exists(dest.c_str());
  if (hadPrior && !Storage.rename(dest.c_str(), backup.c_str())) {
    Storage.removeDir(temp.c_str());
    return false;
  }
  if (!Storage.rename(temp.c_str(), dest.c_str())) {
    if (hadPrior) Storage.rename(backup.c_str(), dest.c_str());
    Storage.removeDir(temp.c_str());
    return false;
  }
  if (hadPrior) Storage.removeDir(backup.c_str());
  LOG_INF("WPK", "Installed wallpaper package %s", release.name.c_str());
  return true;
}

}  // namespace wallpaperpackages
