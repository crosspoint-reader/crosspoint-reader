#include "PackageArchiveInstaller.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ZipFile.h>

#include <cstdlib>
#include <memory>
#include <vector>

#include "PackageStore.h"

namespace Marginalia {

namespace {

constexpr size_t MAX_ARCHIVE_ENTRIES = 96;
constexpr size_t MAX_ARCHIVE_UNCOMPRESSED_BYTES = 512 * 1024;
constexpr size_t MAX_ARCHIVE_ENTRY_BYTES = 128 * 1024;
constexpr size_t ZIP_STREAM_CHUNK_BYTES = 4096;

bool ensureParentDirs(const std::string& baseDir, const std::string& relativePath) {
  auto slash = relativePath.find('/');
  while (slash != std::string::npos) {
    const std::string partial = relativePath.substr(0, slash);
    if (!partial.empty()) {
      const std::string dirPath = baseDir + "/" + partial;
      if (!Storage.ensureDirectoryExists(dirPath.c_str())) {
        return false;
      }
    }
    slash = relativePath.find('/', slash + 1);
  }
  return true;
}

bool archiveMethodSupported(const ZipFile::Entry& entry) { return entry.stat.method == 0 || entry.stat.method == 8; }

PackageArchiveInstallResult resultError(const std::string& error) {
  PackageArchiveInstallResult result;
  result.error = error;
  return result;
}

}  // namespace

PackageArchiveInstallResult extractPackageArchiveToInbox(const std::string& archivePath) {
  if (!ensurePackageBaseDirectories()) {
    return resultError("Could not create package directories");
  }

  ZipFile zip(archivePath);

  size_t manifestSize = 0;
  std::unique_ptr<uint8_t, decltype(&free)> manifestBytes(zip.readFileToMemory("manifest.json", &manifestSize, true),
                                                          free);
  if (!manifestBytes) {
    return resultError("manifest.json missing from archive");
  }
  if (manifestSize == 0 || manifestSize > MAX_MANIFEST_BYTES) {
    return resultError("manifest.json has invalid size");
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, reinterpret_cast<const char*>(manifestBytes.get()));
  if (error) {
    return resultError("manifest.json is invalid");
  }
  if (!doc["id"].is<const char*>()) {
    return resultError("manifest id missing");
  }

  const std::string packageId = doc["id"].as<const char*>();
  if (!isSafePackageId(packageId)) {
    return resultError("invalid package id");
  }

  std::vector<ZipFile::Entry> entries;
  if (!zip.listEntries(entries, MAX_ARCHIVE_ENTRIES)) {
    return resultError("archive entries could not be read");
  }

  size_t totalUncompressed = 0;
  bool hasManifest = false;
  for (const auto& entry : entries) {
    if (entry.isDirectory) continue;
    if (!archiveMethodSupported(entry)) {
      return resultError("archive uses unsupported compression");
    }
    if (!isSafePackageRelativePath(entry.name)) {
      return resultError("archive contains unsafe path");
    }
    if (entry.stat.uncompressedSize > MAX_ARCHIVE_ENTRY_BYTES) {
      return resultError("archive entry is too large");
    }
    totalUncompressed += entry.stat.uncompressedSize;
    if (totalUncompressed > MAX_ARCHIVE_UNCOMPRESSED_BYTES) {
      return resultError("archive is too large");
    }
    if (entry.name == "manifest.json") {
      hasManifest = true;
    }
  }
  if (!hasManifest) {
    return resultError("manifest.json missing from archive");
  }

  const std::string stagingPath = std::string(PACKAGE_STAGING_ROOT) + "/" + packageId + ".archive";
  const std::string inboxPath = std::string(PACKAGE_INBOX_ROOT) + "/" + packageId;

  if (Storage.exists(stagingPath.c_str())) Storage.removeDir(stagingPath.c_str());
  if (!Storage.ensureDirectoryExists(stagingPath.c_str())) {
    return resultError("could not create package staging directory");
  }

  for (const auto& entry : entries) {
    if (entry.isDirectory) continue;
    if (!ensureParentDirs(stagingPath, entry.name)) {
      Storage.removeDir(stagingPath.c_str());
      return resultError("could not create package archive directory");
    }

    const std::string outputPath = stagingPath + "/" + entry.name;
    FsFile output;
    if (!Storage.openFileForWrite("MPKG", outputPath.c_str(), output)) {
      Storage.removeDir(stagingPath.c_str());
      return resultError("could not write package archive entry");
    }
    const bool wroteEntry = zip.readFileToStream(entry.name.c_str(), output, ZIP_STREAM_CHUNK_BYTES);
    output.close();
    if (!wroteEntry) {
      Storage.removeDir(stagingPath.c_str());
      return resultError("could not extract package archive entry");
    }
  }

  PackageStore store;
  PackageManifest manifest = store.readManifest(stagingPath, packageId);
  if (!manifest.valid) {
    Storage.removeDir(stagingPath.c_str());
    return resultError("invalid extracted manifest");
  }
  if (manifest.id != packageId) {
    Storage.removeDir(stagingPath.c_str());
    return resultError("manifest id changed during extraction");
  }
  if (!manifest.compatible) {
    Storage.removeDir(stagingPath.c_str());
    return resultError("incompatible package: " + manifest.compatibilityError);
  }

  if (Storage.exists(inboxPath.c_str())) Storage.removeDir(inboxPath.c_str());
  if (!Storage.rename(stagingPath.c_str(), inboxPath.c_str())) {
    Storage.removeDir(stagingPath.c_str());
    return resultError("could not move package into inbox");
  }

  LOG_DBG("MPKG", "Extracted archive package to inbox: %s", packageId.c_str());

  PackageArchiveInstallResult result;
  result.ok = true;
  result.packageId = manifest.id;
  result.packageName = manifest.name;
  return result;
}

}  // namespace Marginalia
