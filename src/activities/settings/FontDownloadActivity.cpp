#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  // Always silentRestart on exit, regardless of WiFi state. Even if a deep
  // error path turned WiFi off, we still did expensive network/TLS work and
  // the heap is fragmented past the point a normal session can recover (see
  // [project-font-download-heap-stash]). A reboot here gives the next
  // activity a pristine heap.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestart();
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
    previousActionCount_ = actionCount();
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  // Standalone manifest fetch: closes the TLS connection before the JSON
  // parse so the parser has full heap headroom. The Session is opened later
  // for the per-file download loop, on a heap that's been slimmed by
  // trimManifestForDownload().
  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  FsFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read font list";
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (err) {
    LOG_ERR("FONT", "Manifest parse error: %s", err.c_str());
    errorMessage_ = "Invalid font manifest";
    return false;
  }

  int version = doc["version"] | 0;
  // v1 (legacy, no crc32) and v2 (with crc32) are both accepted; crc check is
  // skipped per-file when the field is absent. See upstream PR #1904 for the
  // CRC32 design we mirror.
  if (version != 1 && version != 2) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = "Unsupported manifest version";
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  families_.clear();

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  families_.reserve(familiesArr.size());

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    family.name = fObj["name"] | "";
    family.description = fObj["description"] | "";
    // styles[] in the JSON is intentionally ignored — see ManifestFamily.

    if (!FontInstaller::isValidFamilyName(family.name.c_str())) {
      LOG_ERR("FONT", "Manifest entry rejected, invalid family name: %s", family.name.c_str());
      continue;
    }

    family.totalSize = 0;
    bool fileNamesOk = true;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
      ManifestFile file;
      file.name = fileObj["name"] | "";
      file.size = fileObj["size"] | 0;
      if (fileObj["crc32"].is<uint32_t>()) {
        file.crc32 = fileObj["crc32"].as<uint32_t>();
        file.hasCrc32 = true;
      }
      if (!FontInstaller::isValidFontFileName(file.name.c_str())) {
        LOG_ERR("FONT", "Manifest entry rejected, invalid file name in %s: %s", family.name.c_str(), file.name.c_str());
        fileNamesOk = false;
        break;
      }
      family.totalSize += file.size;
      family.files.push_back(std::move(file));
    }
    if (!fileNamesOk) continue;

    family.installed = fontInstaller_.isFamilyInstalled(family.name.c_str());

    if (family.installed) {
      for (const auto& file : family.files) {
        std::string localFilename = file.name;
        std::string familyPrefix = family.name + "/";
        if (localFilename.find(familyPrefix) == 0) {
          localFilename = localFilename.substr(familyPrefix.length());
        }

        char path[128];
        FontInstaller::buildFontPath(family.name.c_str(), localFilename.c_str(), path, sizeof(path));
        FsFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != file.size) {
            family.hasUpdate = true;
            break;
          }
        } else {
          family.hasUpdate = true;
          break;
        }
      }
    } else {
      // Surface leftover staging from a previously interrupted download so the
      // UI can offer "Resume" instead of restarting from scratch.
      char stagingDir[128];
      FontInstaller::buildStagingDirPath(family.name.c_str(), stagingDir, sizeof(stagingDir));
      family.hasResumableDownload = Storage.exists(stagingDir);
    }

    families_.push_back(std::move(family));
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

// --- Stash/Restore ---
//
// Persist families_ to a small binary file on SD so we can free the
// ~10 KB of scattered std::string allocations it holds. mbedtls's TLS
// handshake needs many small allocations from a defragmented heap; with
// families_ resident, the heap stays fragmented at ~36 KB largest contiguous
// and the handshake fails with -0x2700 / flags=0 (internal alloc failure).
//
// Format (little-endian):
//   u32 magic    = 'CPFM' (0x4D465043)
//   u32 count    = number of families
//   for each family:
//     u8 name_len, name bytes
//     u8 desc_len, desc bytes
//     u32 totalSize
//     u8 flags  bit0 installed, bit1 hasUpdate, bit2 hasResumableDownload
//     u8 file_count
//     for each file:
//       u8 name_len, name bytes
//       u32 size
//       u32 crc32
//       u8 hasCrc32

static constexpr const char* FAMILIES_STASH_PATH = "/fonts_families.bin";
static constexpr uint32_t FAMILIES_STASH_MAGIC = 0x4D465043;  // 'CPFM'

namespace {
bool writeU8(FsFile& f, uint8_t v) { return f.write(&v, 1) == 1; }
bool writeU32(FsFile& f, uint32_t v) {
  uint8_t buf[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                    static_cast<uint8_t>(v >> 24)};
  return f.write(buf, 4) == 4;
}
bool writeStr(FsFile& f, const std::string& s) {
  if (s.size() > 255) return false;
  if (!writeU8(f, static_cast<uint8_t>(s.size()))) return false;
  return s.empty() || f.write(reinterpret_cast<const uint8_t*>(s.data()), s.size()) == s.size();
}
bool readU8(FsFile& f, uint8_t& v) { return f.read(&v, 1) == 1; }
bool readU32(FsFile& f, uint32_t& v) {
  uint8_t buf[4];
  if (f.read(buf, 4) != 4) return false;
  v = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) | (static_cast<uint32_t>(buf[2]) << 16) |
      (static_cast<uint32_t>(buf[3]) << 24);
  return true;
}
bool readStr(FsFile& f, std::string& s) {
  uint8_t len = 0;
  if (!readU8(f, len)) return false;
  s.resize(len);
  if (len == 0) return true;
  return f.read(reinterpret_cast<uint8_t*>(&s[0]), len) == len;
}
}  // namespace

bool FontDownloadActivity::stashFamiliesToSd() {
  Storage.remove(FAMILIES_STASH_PATH);
  FsFile file;
  if (!Storage.openFileForWrite("FONT", FAMILIES_STASH_PATH, file)) {
    LOG_ERR("FONT", "Stash open failed");
    return false;
  }

  bool ok = writeU32(file, FAMILIES_STASH_MAGIC);
  ok = ok && writeU32(file, static_cast<uint32_t>(families_.size()));
  for (const auto& fam : families_) {
    if (!ok) break;
    ok = ok && writeStr(file, fam.name);
    ok = ok && writeStr(file, fam.description);
    ok = ok && writeU32(file, static_cast<uint32_t>(fam.totalSize));
    uint8_t flags = (fam.installed ? 1 : 0) | (fam.hasUpdate ? 2 : 0) | (fam.hasResumableDownload ? 4 : 0);
    ok = ok && writeU8(file, flags);
    ok = ok && writeU8(file, static_cast<uint8_t>(fam.files.size()));
    for (const auto& fl : fam.files) {
      ok = ok && writeStr(file, fl.name);
      ok = ok && writeU32(file, static_cast<uint32_t>(fl.size));
      ok = ok && writeU32(file, fl.crc32);
      ok = ok && writeU8(file, fl.hasCrc32 ? 1 : 0);
    }
  }

  file.flush();
  file.close();
  if (!ok) {
    LOG_ERR("FONT", "Stash write failed");
    Storage.remove(FAMILIES_STASH_PATH);
    return false;
  }
  // Free the in-memory representation now that it's safely on disk.
  families_.clear();
  families_.shrink_to_fit();
  LOG_DBG("FONT", "Stashed families_ to %s and cleared in-memory copy", FAMILIES_STASH_PATH);
  return true;
}

bool FontDownloadActivity::restoreFamiliesFromSd() {
  FsFile file;
  if (!Storage.openFileForRead("FONT", FAMILIES_STASH_PATH, file)) {
    LOG_ERR("FONT", "Stash file missing");
    return false;
  }

  uint32_t magic = 0;
  uint32_t count = 0;
  bool ok = readU32(file, magic) && magic == FAMILIES_STASH_MAGIC && readU32(file, count);
  if (ok) {
    families_.clear();
    families_.reserve(count);
    for (uint32_t i = 0; i < count && ok; i++) {
      ManifestFamily fam;
      ok = ok && readStr(file, fam.name);
      ok = ok && readStr(file, fam.description);
      uint32_t totalSize = 0;
      ok = ok && readU32(file, totalSize);
      fam.totalSize = totalSize;
      uint8_t flags = 0;
      ok = ok && readU8(file, flags);
      fam.installed = (flags & 1) != 0;
      fam.hasUpdate = (flags & 2) != 0;
      fam.hasResumableDownload = (flags & 4) != 0;
      uint8_t fileCount = 0;
      ok = ok && readU8(file, fileCount);
      fam.files.reserve(fileCount);
      for (uint8_t j = 0; j < fileCount && ok; j++) {
        ManifestFile fl;
        ok = ok && readStr(file, fl.name);
        uint32_t fsize = 0, fcrc = 0;
        ok = ok && readU32(file, fsize);
        fl.size = fsize;
        ok = ok && readU32(file, fcrc);
        fl.crc32 = fcrc;
        uint8_t hasCrc = 0;
        ok = ok && readU8(file, hasCrc);
        fl.hasCrc32 = hasCrc != 0;
        fam.files.push_back(std::move(fl));
      }
      if (ok) families_.push_back(std::move(fam));
    }
  }
  file.close();
  if (!ok) {
    LOG_ERR("FONT", "Stash read failed (magic=%08x count=%u)", magic, count);
    return false;
  }
  // Keep the stash file around so a crash mid-download can still recover.
  // It gets overwritten on next stash and is harmless if stale.
  LOG_DBG("FONT", "Restored %zu families from stash", families_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  // Snapshot indices upfront because downloadFamily() stashes/restores
  // families_ — indices remain valid as long as we don't sort or splice it.
  std::vector<int> targetIndices;
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].installed) targetIndices.push_back(static_cast<int>(i));
  }
  for (int idx : targetIndices) {
    downloadFamily(idx);
    if (state_ == ERROR || cancelRequested_) return;
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  std::vector<int> targetIndices;
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed && families_[i].hasUpdate) targetIndices.push_back(static_cast<int>(i));
  }
  for (int idx : targetIndices) {
    downloadFamily(idx);
    if (state_ == ERROR || cancelRequested_) return;
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
}

size_t FontDownloadActivity::totalUninstalledSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed) total += f.totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.installed && f.hasUpdate) total += f.totalSize;
  }
  return total;
}

void FontDownloadActivity::syncSelectedIndexForNewActionCount() {
  const int currentActionCount = actionCount();
  if (currentActionCount == previousActionCount_) {
    return;
  }

  int newIndex = selectedIndex_;
  if (selectedIndex_ >= previousActionCount_) {
    const int familyIndex = selectedIndex_ - previousActionCount_;
    newIndex = familyIndex + currentActionCount;
  } else if (selectedIndex_ >= currentActionCount) {
    newIndex = currentActionCount;
  }

  if (newIndex >= listItemCount()) {
    newIndex = std::max(0, listItemCount() - 1);
  }

  selectedIndex_ = newIndex;
  previousActionCount_ = currentActionCount;
}

bool FontDownloadActivity::hasDownloadCandidates() const {
  for (const auto& f : families_) {
    if (!f.installed) return true;
  }
  return false;
}

bool FontDownloadActivity::hasUpdateCandidates() const {
  for (const auto& f : families_) {
    if (f.installed && f.hasUpdate) return true;
  }
  return false;
}

void FontDownloadActivity::downloadFamily(int familyIdx) {
  if (familyIdx < 0 || familyIdx >= static_cast<int>(families_.size())) {
    LOG_ERR("FONT", "downloadFamily: invalid index %d (size %zu)", familyIdx, families_.size());
    return;
  }

  // Snapshot the target family by value, then stash + free families_ so the
  // ~10 KB of scattered std::string allocations don't fragment the heap
  // during the TLS handshake. Render-path caches (downloadingFamilyName_,
  // downloadingFamilyHasResumable_) cover the family-name and Resume-label
  // accesses that previously read families_ during DOWNLOADING/ERROR.
  ManifestFamily family = families_[familyIdx];
  downloadingFamilyName_ = family.name;
  downloadingFamilyHasResumable_ = family.hasResumableDownload;

  cancelRequested_ = false;
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = familyIdx;
    currentFileIndex_ = 0;
    currentFileTotal_ = family.files.size();
    fileProgress_ = 0;
    fileTotal_ = 0;
  }
  requestUpdateAndWait();

  if (!stashFamiliesToSd()) {
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    errorMessage_ = "Failed to stash manifest";
    return;
  }

  // Run the actual download with families_ empty (defragmented heap).
  downloadFamilyImpl(family, familyIdx);

  // Update cached render state from the impl's mutations.
  downloadingFamilyHasResumable_ = family.hasResumableDownload;

  // Restore families_ regardless of success/error/abort outcome, then merge
  // back the mutations the impl made on the local family copy. Without the
  // restored manifest the activity can't render the family list, so a failed
  // restore is fatal — drop to ERROR rather than continuing with empty state.
  if (!restoreFamiliesFromSd()) {
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    errorMessage_ = "Failed to restore manifest";
    return;
  }
  if (familyIdx >= 0 && familyIdx < static_cast<int>(families_.size())) {
    families_[familyIdx].installed = family.installed;
    families_[familyIdx].hasUpdate = family.hasUpdate;
    families_[familyIdx].hasResumableDownload = family.hasResumableDownload;
  }
  syncSelectedIndexForNewActionCount();
}

void FontDownloadActivity::downloadFamilyImpl(ManifestFamily& family, int familyIdx) {
  // httpSession_ does the TLS handshake on its first downloadToFile call;
  // subsequent files reuse the open keep-alive connection. If the server
  // dropped the connection during the idle gap (user browsing the family
  // list), the Session layer transparently reinitialises and retries once.
  char liveDir[128];
  char stagingDir[128];
  char backupDir[128];
  snprintf(liveDir, sizeof(liveDir), "%s/%s", SdCardFontRegistry::FONTS_DIR, family.name.c_str());
  FontInstaller::buildStagingDirPath(family.name.c_str(), stagingDir, sizeof(stagingDir));
  FontInstaller::buildBackupDirPath(family.name.c_str(), backupDir, sizeof(backupDir));

  // Resume-aware staging: if a __staging dir is left over from a previous
  // interrupted download, keep it so files already on disk can be reused.
  // Files are individually re-verified below (size + CRC) before being
  // accepted, so half-written files are caught.
  if (!Storage.exists(stagingDir) && !Storage.mkdir(stagingDir)) {
    LOG_ERR("FONT", "Failed to create staging dir: %s", stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to create staging area";
    return;
  }

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
      fileProgress_ = 0;
      fileTotal_ = file.size;
      lastProgressPercent_ = -1;
      lastProgressUpdateMs_ = 0;
    }
    requestUpdateAndWait();

    std::string localFilename = file.name;
    std::string familyPrefix = family.name + "/";
    if (localFilename.find(familyPrefix) == 0) {
      localFilename = localFilename.substr(familyPrefix.length());
    }

    char stagedPath[128];
    snprintf(stagedPath, sizeof(stagedPath), "%s/%s", stagingDir, localFilename.c_str());

    // If this file is already present in staging from a previous run and
    // matches the manifest, skip the download. CRC32 is checked when the
    // manifest carries one (v2+); otherwise size + magic-byte check is the
    // best we can do.
    if (Storage.exists(stagedPath)) {
      FsFile f;
      bool sizeOk = false;
      if (Storage.openFileForRead("FONT", stagedPath, f)) {
        sizeOk = (f.fileSize() == file.size);
        f.close();
      }
      bool crcOk = !file.hasCrc32;
      if (sizeOk && file.hasCrc32) {
        uint32_t actualCrc = 0;
        if (FontInstaller::computeFileCrc32(stagedPath, actualCrc)) {
          crcOk = (actualCrc == file.crc32);
        }
      }
      if (sizeOk && crcOk && fontInstaller_.validateCpfontFile(stagedPath)) {
        LOG_DBG("FONT", "Resuming: reusing %s", stagedPath);
        fileProgress_ = file.size;
        fileTotal_ = file.size;
        continue;
      }
      LOG_DBG("FONT", "Resuming: re-downloading stale %s (sizeOk=%d crcOk=%d)", stagedPath, sizeOk, crcOk);
      Storage.remove(stagedPath);
    }

    // Make sure parent directories exist for the file
    std::string stagedPathStr(stagedPath);
    size_t lastSlash = stagedPathStr.find_last_of('/');
    if (lastSlash != std::string::npos) {
      Storage.mkdir(stagedPathStr.substr(0, lastSlash).c_str());
    }

    std::string url = baseUrl_ + file.name;

    auto result = HttpDownloader::downloadToFile(
        httpSession_, url, stagedPath, [this](unsigned int downloaded, unsigned int total) {
          mappedInput.update();
          fileProgress_ = downloaded;
          fileTotal_ = total;

          const unsigned long now = millis();
          int percent = 0;
          if (total > 0) {
            percent = static_cast<int>((static_cast<unsigned long long>(downloaded) * 100ULL + total / 2) / total);
          }
          const bool percentChanged = percent != lastProgressPercent_;
          const bool timeElapsed = lastProgressUpdateMs_ == 0 || now - lastProgressUpdateMs_ > 2000;
          if ((percentChanged && timeElapsed) || downloaded == total) {
            requestUpdate(true);
            lastProgressPercent_ = percent;
            lastProgressUpdateMs_ = now;
          }

          return !mappedInput.wasPressed(MappedInputManager::Button::Back);
        });

    if (result == HttpDownloader::ABORTED) {
      LOG_INF("FONT", "Download cancelled: %s", file.name.c_str());
      // Keep staging dir so the next launch can resume.
      Storage.remove(stagedPath);
      family.hasResumableDownload = !family.installed;
      cancelRequested_ = true;
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      // Drop just the file that failed; keep already-downloaded siblings so
      // the next retry resumes from here.
      Storage.remove(stagedPath);
      family.hasResumableDownload = !family.installed;
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = familyIdx;
      errorMessage_ = "Download failed: " + file.name;
      return;
    }

    // CRC32: matches upstream PR #1904 — catches truncated/torn writes.
    if (file.hasCrc32) {
      uint32_t actualCrc = 0;
      if (!FontInstaller::computeFileCrc32(stagedPath, actualCrc)) {
        LOG_ERR("FONT", "Failed to read for CRC: %s", stagedPath);
        Storage.remove(stagedPath);
        family.hasResumableDownload = !family.installed;
        RenderLock lock(*this);
        state_ = ERROR;
        pendingErrorAction_ = PendingFontAction::Download;
        downloadingFamilyIndex_ = familyIdx;
        errorMessage_ = "Failed to verify: " + file.name;
        return;
      }
      if (actualCrc != file.crc32) {
        LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
        Storage.remove(stagedPath);
        family.hasResumableDownload = !family.installed;
        RenderLock lock(*this);
        state_ = ERROR;
        pendingErrorAction_ = PendingFontAction::Download;
        downloadingFamilyIndex_ = familyIdx;
        errorMessage_ = "Checksum mismatch: " + file.name;
        return;
      }
    }

    if (!fontInstaller_.validateCpfontFile(stagedPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", stagedPath);
      Storage.remove(stagedPath);
      family.hasResumableDownload = !family.installed;
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = familyIdx;
      errorMessage_ = "Invalid font file: " + file.name;
      return;
    }
  }

  const bool hadLiveDir = Storage.exists(liveDir);

  if (Storage.exists(backupDir) && !Storage.removeDir(backupDir)) {
    LOG_ERR("FONT", "Failed to clean backup dir: %s", backupDir);
    Storage.removeDir(stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to prepare backup area";
    return;
  }

  if (hadLiveDir && !Storage.rename(liveDir, backupDir)) {
    LOG_ERR("FONT", "Failed to move live family to backup: %s", liveDir);
    Storage.removeDir(stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to replace installed font";
    return;
  }

  if (!Storage.rename(stagingDir, liveDir)) {
    LOG_ERR("FONT", "Failed to activate staged family: %s", stagingDir);
    if (hadLiveDir && Storage.exists(backupDir)) {
      Storage.rename(backupDir, liveDir);
    }
    Storage.removeDir(stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to finalize font install";
    return;
  }

  if (Storage.exists(backupDir) && !Storage.removeDir(backupDir)) {
    LOG_INF("FONT", "Failed to remove backup dir after successful install: %s", backupDir);
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;
  family.hasResumableDownload = false;
  // syncSelectedIndexForNewActionCount() is deferred to downloadFamily() —
  // it needs families_ which is empty during this impl.

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::promptDeleteFamily(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
  const auto& family = families_[familyIndex];
  const std::string heading = tr(STR_DELETE) + std::string("?");
  const std::string body = family.name;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this, familyIndex](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           deleteFamilyAtIndex(familyIndex);
                         });
}

void FontDownloadActivity::deleteFamilyAtIndex(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;

  auto& family = families_[familyIndex];
  const auto result = fontInstaller_.deleteFamily(family.name.c_str());
  if (result == FontInstaller::Error::OK) {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    syncSelectedIndexForNewActionCount();
    pendingErrorAction_ = PendingFontAction::None;
    errorMessage_.clear();

    if (selectedIndex_ >= listItemCount()) {
      selectedIndex_ = std::max(0, listItemCount() - 1);
    }

    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    requestUpdate();
    return;
  }

  std::string message = "Failed to delete font";
  if (result == FontInstaller::Error::INVALID_FAMILY_NAME) {
    message = "Invalid font family";
  }

  RenderLock lock(*this);
  state_ = ERROR;
  downloadingFamilyIndex_ = familyIndex;
  pendingErrorAction_ = PendingFontAction::Delete;
  errorMessage_ = message;
}

std::string FontDownloadActivity::confirmButtonLabel() const {
  if (families_.empty()) return tr(STR_DOWNLOAD);
  if (isDownloadAllSelected()) return tr(STR_DOWNLOAD);
  if (isUpdateAllSelected()) return tr(STR_UPDATE);
  const auto& family = families_[familyIndexFromList(selectedIndex_)];
  if (family.installed && !family.hasUpdate) return tr(STR_DELETE);
  if (family.hasUpdate) return tr(STR_UPDATE);
  if (family.hasResumableDownload) return tr(STR_RESUME);
  return tr(STR_DOWNLOAD);
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    syncSelectedIndexForNewActionCount();
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator_.onNextList(selectedIndex_, listItemCount(), [this] { requestUpdate(); });
    buttonNavigator_.onPreviousList(selectedIndex_, listItemCount(), [this] { requestUpdate(); });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        if (isDownloadAllSelected()) {
          downloadAll();
          requestUpdateAndWait();
        } else if (isUpdateAllSelected()) {
          updateAll();
          requestUpdateAndWait();
        } else {
          const int familyIndex = familyIndexFromList(selectedIndex_);
          const auto& family = families_[familyIndex];
          if (family.installed && !family.hasUpdate) {
            promptDeleteFamily(familyIndex);
          } else {
            downloadFamily(familyIndex);
            requestUpdateAndWait();
          }
        }
      }
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        if (pendingErrorAction_ == PendingFontAction::Delete) {
          deleteFamilyAtIndex(downloadingFamilyIndex_);
        } else {
          downloadFamily(downloadingFamilyIndex_);
        }
        requestUpdateAndWait();
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_MANAGER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    syncSelectedIndexForNewActionCount();
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (hasDownloadCandidates()) {
              if (index == 0) {
                return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalUninstalledSize()) + ")";
              }
              if (hasUpdateCandidates() && index == 1) {
                return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
              }
            } else if (hasUpdateCandidates() && index == 0) {
              return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          [this](int index) -> std::string {
            if (hasDownloadCandidates()) {
              if (index == 0) return "";
              if (hasUpdateCandidates() && index == 1) return "";
            } else if (hasUpdateCandidates() && index == 0) {
              return "";
            }
            return families_[familyIndexFromList(index)].description;
          },
          nullptr,
          [this](int index) -> std::string {
            if (hasDownloadCandidates()) {
              if (index == 0) return "";
              if (hasUpdateCandidates() && index == 1) return "";
            } else if (hasUpdateCandidates() && index == 0) {
              return "";
            }
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            if (f.hasResumableDownload) return tr(STR_RESUME);
            return "";
          },
          true);

      const std::string confirmLabel = confirmButtonLabel();
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel.c_str(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    // families_ is stashed to SD during downloadFamily(); read the cached
    // name instead of indexing families_.
    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + downloadingFamilyName_ + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    int percentY = barY + metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, percentY,
                              (std::to_string(static_cast<int>(progress * 100)) + "%").c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    // Use the cached value: families_ may have just been restored (post-impl)
    // or still empty (if the failure was in the stash itself); either way the
    // cache reflects the last update from the download attempt.
    const bool canResume = pendingErrorAction_ == PendingFontAction::Download && downloadingFamilyHasResumable_;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), canResume ? tr(STR_RESUME) : tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
