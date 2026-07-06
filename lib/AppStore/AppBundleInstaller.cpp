#include "AppBundleInstaller.h"

#include <AppDateTimeFormat.h>
#include <AppPathSanitizer.h>
#include <AppRegistry.h>
#include <AppStorePaths.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipFile.h>

#include <cstring>

namespace {

constexpr size_t kMaxBundleFiles = 48;
constexpr size_t kMaxBundleEntryLen = 96;

struct BundleFileEntry {
  char zipPath[kMaxBundleEntryLen];
  char destPath[128];
};

bool ensureParentDirectoryExists(const char* filePath) {
  const char* lastSlash = strrchr(filePath, '/');
  if (lastSlash == nullptr || lastSlash == filePath) {
    return true;
  }
  char dirPath[128];
  const size_t len = static_cast<size_t>(lastSlash - filePath);
  if (len >= sizeof(dirPath)) {
    return false;
  }
  memcpy(dirPath, filePath, len);
  dirPath[len] = '\0';
  if (Storage.exists(dirPath)) {
    return true;
  }
  return Storage.mkdir(dirPath, true);
}

bool extractZipEntry(ZipFile& zip, const char* zipEntry, const char* destPath) {
  if (!ensureParentDirectoryExists(destPath)) {
    return false;
  }

  HalFile outFile;
  if (!Storage.openFileForWrite("APPS", destPath, outFile)) {
    LOG_ERR("APPS", "Install failed: could not open %s for write", destPath);
    return false;
  }

  const bool ok = zip.readFileToStream(zipEntry, outFile, 1024);
  outFile.close();
  if (!ok) {
    Storage.remove(destPath);
  }
  return ok;
}

}  // namespace

bool AppBundleInstaller::installFromZipFile(const char* zipPath, const AppInstallRequest& request) {
  if (!AppPathSanitizer::isValidAppId(request.id)) {
    LOG_ERR("APPS", "Install rejected: invalid app id %s", request.id.c_str());
    return false;
  }

  Storage.ensureDirectoryExists(AppStorePaths::kTmpDir);
  Storage.ensureDirectoryExists(AppStorePaths::kAppsRoot);

  char appDirPath[96];
  snprintf(appDirPath, sizeof(appDirPath), "%s/%s", AppStorePaths::kAppsRoot, request.id.c_str());

  if (Storage.exists(appDirPath)) {
    if (!Storage.removeDir(appDirPath)) {
      LOG_ERR("APPS", "Install failed: could not remove existing app dir %s", appDirPath);
      return false;
    }
  }
  Storage.ensureDirectoryExists(appDirPath);

  ZipFile zip(zipPath);
  bool extractOk = true;
  bool hasMainLua = false;
  bool hasManifest = false;
  auto bundleFiles = makeUniqueNoThrow<BundleFileEntry[]>(kMaxBundleFiles);
  if (!bundleFiles) {
    LOG_ERR("APPS", "Install failed: OOM bundle file table");
    return false;
  }
  size_t bundleFileCount = 0;

  zip.enumerateFilePaths([&](const std::string_view entryPath) {
    if (!extractOk) {
      return;
    }

    if (entryPath.empty() || entryPath.back() == '/') {
      return;
    }

    const auto sanitized = AppPathSanitizer::sanitizeRelativePath(entryPath);
    if (!sanitized.has_value()) {
      LOG_ERR("APPS", "Install rejected: unsafe bundle path %.*s", static_cast<int>(entryPath.size()), entryPath.data());
      extractOk = false;
      return;
    }

    if (sanitized->normalized == "main.lua") {
      hasMainLua = true;
    } else if (sanitized->normalized == "manifest.json") {
      hasManifest = true;
    }

    if (bundleFileCount >= kMaxBundleFiles) {
      LOG_ERR("APPS", "Install failed: bundle has too many files");
      extractOk = false;
      return;
    }

    if (entryPath.size() >= kMaxBundleEntryLen) {
      LOG_ERR("APPS", "Install failed: bundle entry path too long");
      extractOk = false;
      return;
    }

    BundleFileEntry& fileEntry = bundleFiles[bundleFileCount];
    memcpy(fileEntry.zipPath, entryPath.data(), entryPath.size());
    fileEntry.zipPath[entryPath.size()] = '\0';

    const int destWritten =
        snprintf(fileEntry.destPath, sizeof(fileEntry.destPath), "%s/%s", appDirPath, sanitized->normalized.c_str());
    if (destWritten <= 0 || static_cast<size_t>(destWritten) >= sizeof(fileEntry.destPath)) {
      LOG_ERR("APPS", "Install failed: path too long for %s", sanitized->normalized.c_str());
      extractOk = false;
      return;
    }

    bundleFileCount++;
  });

  for (size_t i = 0; extractOk && i < bundleFileCount; i++) {
    const BundleFileEntry& fileEntry = bundleFiles[i];
    if (!extractZipEntry(zip, fileEntry.zipPath, fileEntry.destPath)) {
      LOG_ERR("APPS", "Install failed: could not extract %s", fileEntry.zipPath);
      extractOk = false;
      break;
    }
  }

  if (!extractOk || !hasMainLua || !hasManifest) {
    if (!hasMainLua || !hasManifest) {
      LOG_ERR("APPS", "Install failed: bundle missing required files");
    }
    Storage.removeDir(appDirPath);
    return false;
  }

  AppRegistryEntry registryEntry;
  registryEntry.id = request.id;
  registryEntry.name = request.name;
  registryEntry.version = request.version;
  registryEntry.installedAt =
      request.installedAt.empty() ? AppDateTimeFormat::formatNowIso8601Utc() : request.installedAt;
  if (!APP_REGISTRY.upsertEntry(registryEntry)) {
    LOG_ERR("APPS", "Install failed: could not update registry for %s", request.id.c_str());
    Storage.removeDir(appDirPath);
    return false;
  }

  LOG_INF("APPS", "Installed app id=%s version=%s", request.id.c_str(), request.version.c_str());
  return true;
}
