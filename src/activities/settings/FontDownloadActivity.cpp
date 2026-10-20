#include "FontDownloadActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SerialTestApi.h"
#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/FontManifestReader.h"

namespace fui = freeink::ui;

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();
  // The heap gate for this screen (PR 5 Task 8): the activity now carries a UiAppHost, and the
  // TLS handshake below is the allocation that cannot afford it.
  LOG_DBG("FONT", "Activity %u B, of which UiAppHost %u B", static_cast<unsigned>(sizeof(*this)),
          static_cast<unsigned>(sizeof(::UiAppHost)));

  // Free the heap the WiFi stack needs before it is brought up, not after -
  // association itself is the allocation-heavy step, well ahead of TLS. That
  // includes the per-category SettingInfo vectors of the SettingsActivity below
  // us, which fragment the heap; onExit() reboots, so they are never needed again.
  // WifiSelectionActivity sets WIFI_STA itself, so no radio work happens here.
  releaseMemoryForDownload(renderer, "FONT");

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  // Routing closes first (UiListActivity::onExit()); the restart below never returns.
  UiListActivity::onExit();

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

  // Re-trim: the status screens rendered since onEnter can have repopulated the
  // font cache. Idempotent - the secondary buffer is already gone by now.
  releaseMemoryForDownload(renderer, "FONT");

  const bool loaded = fetchAndParseManifest();
  // The fetch held the loop task. Nothing pressed meanwhile may reach the list.
  buttonEvents.drain();
  if (!loaded) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    refreshRowTotals();
    previousActionCount_ = actionCount();
    state_ = FAMILY_LIST;
  }
  requestUpdate();
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  // Standalone manifest fetch: closes the TLS connection before the manifest
  // is read, so the reader has full heap headroom. The Session is opened later
  // for the per-file download loop, after the font list has been stashed to
  // the card.
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

  // Streamed off the card a block at a time: a whole-document parse of the
  // 24 KB manifest ran the X3 out of heap and aborted. styles[] in the JSON
  // is skipped -- see families_. Read aside and swapped in under the lock
  // below: the render task reads families_.
  FontCatalog parsed;
  FontManifestReader reader("FONT", FontInstaller::isValidFamilyName, FontInstaller::isValidFontFileName);
  const FontManifestStatus status = reader.read(manifestFile, parsed, baseUrl_);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (status == FontManifestStatus::Invalid) {
    LOG_ERR("FONT", "Manifest parse error: %s", reader.failure());
    errorMessage_ = "Invalid font manifest";
    return false;
  }
  if (status == FontManifestStatus::UnsupportedVersion) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", reader.version());
    errorMessage_ = "Unsupported manifest version";
    return false;
  }
  if (status == FontManifestStatus::OutOfMemory) {
    LOG_ERR("FONT", "Manifest parse error: %s", reader.failure());
    errorMessage_ = "Failed to read font list";
    return false;
  }

  for (size_t i = 0; i < parsed.count(); i++) {
    const char* familyName = parsed.name(i);
    parsed.setInstalled(i, fontInstaller_.isFamilyInstalled(familyName));

    if (parsed.installed(i)) {
      for (size_t j = 0; j < parsed.fileCount(i); j++) {
        char path[128];
        FontInstaller::buildFontPath(familyName, parsed.fileLocalName(i, j), path, sizeof(path));
        FsFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != parsed.fileSize(i, j)) {
            parsed.setHasUpdate(i, true);
            break;
          }
        } else {
          parsed.setHasUpdate(i, true);
          break;
        }
      }
    } else {
      // Surface leftover staging from a previously interrupted download so the
      // UI can offer "Resume" instead of restarting from scratch.
      char stagingDir[128];
      FontInstaller::buildStagingDirPath(familyName, stagingDir, sizeof(stagingDir));
      parsed.setHasResumableDownload(i, Storage.exists(stagingDir));
    }
  }

  {
    RenderLock lock(*this);
    families_.swap(parsed);
    refreshRowTotals();
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families, %zu files, %zu bytes in one block", families_.count(),
          families_.totalFiles(), families_.blockBytes());
  return true;
}

// --- Stash/Restore ---
//
// Persist families_ to SD so its block can be freed for the download. The
// wolfSSL session needs a contiguous ~17 KB block for a TLS record. The list
// used to be ~300 scattered allocations, and freeing them left the heap in
// pieces too small for it (X3: largest block 10 KB, MEMORY_E, download
// failed); its one block now comes back whole. The file is the block itself,
// as FontCatalog documents it, written in one write and read back in one read.

static constexpr const char* FAMILIES_STASH_PATH = "/fonts_families.bin";

bool FontDownloadActivity::stashFamiliesToSd() {
  Storage.remove(FAMILIES_STASH_PATH);
  FsFile file;
  if (!Storage.openFileForWrite("FONT", FAMILIES_STASH_PATH, file)) {
    LOG_ERR("FONT", "Stash open failed");
    return false;
  }

  const bool ok = families_.writeTo(file);
  file.flush();
  file.close();
  if (!ok) {
    LOG_ERR("FONT", "Stash write failed");
    Storage.remove(FAMILIES_STASH_PATH);
    return false;
  }
  // Free the in-memory copy now that it's safely on disk. Under the lock: the
  // render task reads the block's strings.
  const size_t bytes = families_.blockBytes();
  {
    RenderLock lock(*this);
    families_.clear();
  }
  LOG_DBG("FONT", "Stashed families_ (%zu bytes) to %s and cleared in-memory copy", bytes, FAMILIES_STASH_PATH);
  return true;
}

bool FontDownloadActivity::restoreFamiliesFromSd(FontCatalog& out) {
  FsFile file;
  if (!Storage.openFileForRead("FONT", FAMILIES_STASH_PATH, file)) {
    LOG_ERR("FONT", "Stash file missing");
    return false;
  }

  // Into a block of its own; the caller hands it over under the lock, so the
  // render task never sees a half-read list.
  const size_t fileBytes = file.fileSize();
  const bool ok = out.readFrom(file);
  file.close();
  if (!ok) {
    LOG_ERR("FONT", "Stash read failed (%zu bytes)", fileBytes);
    return false;
  }
  // Keep the stash file around so a crash mid-download can still recover.
  // It gets overwritten on next stash and is harmless if stale.
  LOG_DBG("FONT", "Restored %zu families from stash", out.count());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  // Snapshot indices upfront because downloadFamily() stashes/restores
  // families_ — indices remain valid as long as we don't sort or splice it.
  std::vector<int> targetIndices;
  targetIndices.reserve(families_.count());
  for (size_t i = 0; i < families_.count(); i++) {
    if (!families_.installed(i)) targetIndices.push_back(static_cast<int>(i));
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
  targetIndices.reserve(families_.count());
  for (size_t i = 0; i < families_.count(); i++) {
    if (families_.installed(i) && families_.hasUpdate(i)) targetIndices.push_back(static_cast<int>(i));
  }
  for (int idx : targetIndices) {
    downloadFamily(idx);
    if (state_ == ERROR || cancelRequested_) return;
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::refreshRowTotals() {
  size_t uninstalledBytes = 0;
  size_t updateBytes = 0;
  uninstalledCount_ = 0;
  updateCount_ = 0;
  for (size_t i = 0; i < families_.count(); i++) {
    if (!families_.installed(i)) {
      ++uninstalledCount_;
      uninstalledBytes += families_.totalSize(i);
    } else if (families_.hasUpdate(i)) {
      ++updateCount_;
      updateBytes += families_.totalSize(i);
    }
  }
  char size[32];
  formatSize(uninstalledBytes, size, sizeof(size));
  snprintf(downloadAllLabel_, sizeof(downloadAllLabel_), "%s (%s)", tr(STR_DOWNLOAD_ALL), size);
  formatSize(updateBytes, size, sizeof(size));
  snprintf(updateAllLabel_, sizeof(updateAllLabel_), "%s (%s)", tr(STR_UPDATE_ALL), size);
}

// The action rows come and go with what is left to download or update, and every family row
// shifts with them. Keep the selection on the same family, or on the first family when the
// selected action row went. The selection is the nav's: requested from the loop task, never
// written by the render.
void FontDownloadActivity::syncSelectedIndexForNewActionCount() {
  const int currentActionCount = actionCount();
  if (currentActionCount == previousActionCount_) {
    return;
  }

  auto& current = activeNav();
  const int selected = current.selected.load();
  int newIndex = selected;
  if (selected >= previousActionCount_) {
    newIndex = selected - previousActionCount_ + currentActionCount;
  } else if (selected >= currentActionCount) {
    newIndex = currentActionCount;
  }
  newIndex = std::min(newIndex, std::max(0, listItemCount() - 1));

  previousActionCount_ = currentActionCount;
  if (newIndex != selected) current.requestSelection(newIndex);
}

void FontDownloadActivity::downloadFamily(int familyIdx) {
  if (familyIdx < 0 || familyIdx >= static_cast<int>(families_.count())) {
    LOG_ERR("FONT", "downloadFamily: invalid index %d (size %zu)", familyIdx, families_.count());
    return;
  }

  // Copy the target family into a block of its own (a few hundred bytes),
  // then stash + free families_ so the TLS session finds the heap the list
  // held in one piece. Render-path caches (downloadingFamilyName_,
  // downloadingFamilyHasResumable_) cover the family-name and Resume-label
  // accesses that previously read families_ during DOWNLOADING/ERROR.
  FontCatalog family;
  if (!family.copyFamilyFrom(families_, familyIdx)) {
    LOG_ERR("FONT", "OOM: copy of family %s", families_.name(familyIdx));
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = familyIdx;
    errorMessage_ = "Failed to stash manifest";
    return;
  }

  cancelRequested_ = false;
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    snprintf(downloadingFamilyName_, sizeof(downloadingFamilyName_), "%s", family.name(0));
    downloadingFamilyHasResumable_ = family.hasResumableDownload(0);
    downloadingFamilyIndex_ = familyIdx;
    currentFileIndex_ = 0;
    currentFileTotal_ = family.fileCount(0);
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
  // The list screens drew since the manifest loaded: give their glyph caches back too.
  releaseMemoryForDownload(renderer, "FONT");

  // Run the actual download with families_ empty (defragmented heap).
  downloadFamilyImpl(family, familyIdx);
  SerialTestApi::noteFontInstalled(family.name(0), state_ == COMPLETE);
  // The transfer held the loop task. Its progress callback read Back as an event and dropped every
  // other press; clear whatever is still half-made, so nothing pressed during the transfer reaches
  // the list as a step or a second Confirm.
  buttonEvents.drain();

  // Restore families_ regardless of success/error/abort outcome, then merge
  // back the mutations the impl made on the local family copy. Without the
  // restored manifest the activity can't render the family list, so a failed
  // restore is fatal — drop to ERROR rather than continuing with empty state.
  // Read outside the lock, swapped in under it: the render task reads families_.
  FontCatalog restored;
  const bool restoredOk = restoreFamiliesFromSd(restored);

  RenderLock lock(*this);
  // Update cached render state from the impl's mutations.
  downloadingFamilyHasResumable_ = family.hasResumableDownload(0);
  if (!restoredOk) {
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    errorMessage_ = "Failed to restore manifest";
    return;
  }
  families_.swap(restored);
  if (familyIdx < static_cast<int>(families_.count())) {
    families_.setInstalled(familyIdx, family.installed(0));
    families_.setHasUpdate(familyIdx, family.hasUpdate(0));
    families_.setHasResumableDownload(familyIdx, family.hasResumableDownload(0));
  }
  refreshRowTotals();
  syncSelectedIndexForNewActionCount();
  // A cancelled transfer returns to the list now that the list is back.
  if (cancelRequested_) state_ = FAMILY_LIST;
}

void FontDownloadActivity::downloadFamilyImpl(FontCatalog& family, int familyIdx) {
  // `family` holds this one family, at index 0.
  const char* familyName = family.name(0);

  // httpSession_ does the TLS handshake on its first downloadToFile call;
  // subsequent files reuse the open keep-alive connection. If the server
  // dropped the connection during the idle gap (user browsing the family
  // list), the Session layer transparently reinitialises and retries once.
  char liveDir[128];
  char stagingDir[128];
  char backupDir[128];
  snprintf(liveDir, sizeof(liveDir), "%s/%s", SdCardFontRegistry::FONTS_DIR, familyName);
  FontInstaller::buildStagingDirPath(familyName, stagingDir, sizeof(stagingDir));
  FontInstaller::buildBackupDirPath(familyName, backupDir, sizeof(backupDir));

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

  for (size_t i = 0; i < family.fileCount(0); i++) {
    const char* fileName = family.fileName(0, i);
    const uint32_t expectedSize = family.fileSize(0, i);
    const bool hasCrc32 = family.fileHasCrc32(0, i);
    const uint32_t expectedCrc32 = family.fileCrc32(0, i);

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
      fileProgress_ = 0;
      fileTotal_ = expectedSize;
      lastProgressPercent_ = -1;
      lastProgressUpdateMs_ = 0;
    }
    requestUpdateAndWait();

    char stagedPath[128];
    snprintf(stagedPath, sizeof(stagedPath), "%s/%s", stagingDir, family.fileLocalName(0, i));

    // If this file is already present in staging from a previous run and
    // matches the manifest, skip the download. CRC32 is checked when the
    // manifest carries one (v2+); otherwise size + magic-byte check is the
    // best we can do.
    if (Storage.exists(stagedPath)) {
      FsFile f;
      bool sizeOk = false;
      if (Storage.openFileForRead("FONT", stagedPath, f)) {
        sizeOk = (f.fileSize() == expectedSize);
        f.close();
      }
      bool crcOk = !hasCrc32;
      if (sizeOk && hasCrc32) {
        uint32_t actualCrc = 0;
        if (FontInstaller::computeFileCrc32(stagedPath, actualCrc)) {
          crcOk = (actualCrc == expectedCrc32);
        }
      }
      if (sizeOk && crcOk && fontInstaller_.validateCpfontFile(stagedPath)) {
        LOG_DBG("FONT", "Resuming: reusing %s", stagedPath);
        fileProgress_ = expectedSize;
        fileTotal_ = expectedSize;
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

    std::string url = baseUrl_ + fileName;

    auto result = HttpDownloader::downloadToFile(
        httpSession_, url, stagedPath, [this](unsigned int downloaded, unsigned int total) {
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

          return !backPressedDuringTransfer();
        });

    if (result == HttpDownloader::ABORTED) {
      LOG_INF("FONT", "Download cancelled: %s", fileName);
      // Keep staging dir so the next launch can resume.
      Storage.remove(stagedPath);
      family.setHasResumableDownload(0, !family.installed(0));
      // downloadFamily() returns to the list once families_ is restored; until then the list has
      // nothing to show.
      cancelRequested_ = true;
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", fileName, result);
      // Drop just the file that failed; keep already-downloaded siblings so
      // the next retry resumes from here.
      Storage.remove(stagedPath);
      family.setHasResumableDownload(0, !family.installed(0));
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = familyIdx;
      errorMessage_ = std::string("Download failed: ") + fileName;
      return;
    }

    // CRC32: matches upstream PR #1904 — catches truncated/torn writes.
    if (hasCrc32) {
      uint32_t actualCrc = 0;
      if (!FontInstaller::computeFileCrc32(stagedPath, actualCrc)) {
        LOG_ERR("FONT", "Failed to read for CRC: %s", stagedPath);
        Storage.remove(stagedPath);
        family.setHasResumableDownload(0, !family.installed(0));
        RenderLock lock(*this);
        state_ = ERROR;
        pendingErrorAction_ = PendingFontAction::Download;
        downloadingFamilyIndex_ = familyIdx;
        errorMessage_ = std::string("Failed to verify: ") + fileName;
        return;
      }
      if (actualCrc != expectedCrc32) {
        LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", fileName, actualCrc, expectedCrc32);
        Storage.remove(stagedPath);
        family.setHasResumableDownload(0, !family.installed(0));
        RenderLock lock(*this);
        state_ = ERROR;
        pendingErrorAction_ = PendingFontAction::Download;
        downloadingFamilyIndex_ = familyIdx;
        errorMessage_ = std::string("Checksum mismatch: ") + fileName;
        return;
      }
    }

    if (!fontInstaller_.validateCpfontFile(stagedPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", stagedPath);
      Storage.remove(stagedPath);
      family.setHasResumableDownload(0, !family.installed(0));
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = familyIdx;
      errorMessage_ = std::string("Invalid font file: ") + fileName;
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
  family.setInstalled(0, true);
  family.setHasUpdate(0, false);
  family.setHasResumableDownload(0, false);
  // syncSelectedIndexForNewActionCount() is deferred to downloadFamily() —
  // it needs families_ which is empty during this impl.

  RenderLock lock(*this);
  state_ = COMPLETE;
}

// The transfer holds the loop task, so main.cpp's loop is not turning presses into events while
// it runs. Pump them here in main.cpp's order (sample the keys, then classify the edges) and
// report Back, whatever kind of press it was. Every other press is dropped, and downloadFamily()
// drains what is left once the transfer returns.
bool FontDownloadActivity::backPressedDuringTransfer() {
  mappedInput.update();
  buttonEvents.update();
  bool back = false;
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    if (event.button == MappedInputManager::Button::Back) back = true;
  }
  return back;
}

void FontDownloadActivity::promptDeleteFamily(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.count())) return;
  const std::string body = families_.name(familyIndex);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_QUESTION), body),
                         [this, familyIndex](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           deleteFamilyAtIndex(familyIndex);
                         });
}

void FontDownloadActivity::deleteFamilyAtIndex(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.count())) return;

  const auto result = fontInstaller_.deleteFamily(families_.name(familyIndex));
  if (result == FontInstaller::Error::OK) {
    fontInstaller_.refreshRegistry();
    {
      RenderLock lock(*this);
      families_.setInstalled(familyIndex, false);
      families_.setHasUpdate(familyIndex, false);
      refreshRowTotals();
      syncSelectedIndexForNewActionCount();
      pendingErrorAction_ = PendingFontAction::None;
      errorMessage_.clear();
      state_ = FAMILY_LIST;
    }
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

// --- The family list ---

const char* FontDownloadActivity::headerTitle() const { return tr(STR_FONT_MANAGER); }

const char* FontDownloadActivity::footerConfirmLabel() const {
  if (state_ != FAMILY_LIST || families_.empty()) return "";
  const int selected = nav.selected.load();
  if (isDownloadAllRow(selected)) return tr(STR_DOWNLOAD);
  if (isUpdateAllRow(selected)) return tr(STR_UPDATE);
  const int familyIndex = familyIndexFromList(selected);
  if (familyIndex < 0) return "";
  if (families_.installed(familyIndex) && !families_.hasUpdate(familyIndex)) return tr(STR_DELETE);
  if (families_.hasUpdate(familyIndex)) return tr(STR_UPDATE);
  if (families_.hasResumableDownload(familyIndex)) return tr(STR_RESUME);
  return tr(STR_DOWNLOAD);
}

void FontDownloadActivity::activateIndex(const int index) {
  if (state_ != FAMILY_LIST) return;
  // Activation starts a download or opens the delete prompt; a lingering tap flash would gray an
  // unrelated row once the list comes back.
  app.clearTapFlash();
  if (isDownloadAllRow(index)) {
    downloadAll();
  } else if (isUpdateAllRow(index)) {
    updateAll();
  } else {
    const int familyIndex = familyIndexFromList(index);
    if (familyIndex < 0) return;
    if (families_.installed(familyIndex) && !families_.hasUpdate(familyIndex)) {
      promptDeleteFamily(familyIndex);
      return;
    }
    downloadFamily(familyIndex);
  }
  requestUpdateAndWait();
}

bool FontDownloadActivity::handleCustomInput() {
  // The family list is the base's: its controller, its touch rows, its swipe.
  if (state_ == FAMILY_LIST) return false;

  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    const bool back = event.button == MappedInputManager::Button::Back;
    const bool confirm = event.button == MappedInputManager::Button::Confirm;
    if ((state_ == COMPLETE && (back || confirm)) || (state_ == ERROR && back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
      return true;
    }
    if (state_ == ERROR && confirm) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.count())) {
        if (pendingErrorAction_ == PendingFontAction::Delete) {
          deleteFamilyAtIndex(downloadingFamilyIndex_);
          // The delete held the loop task; presses made meanwhile must not reach the list.
          buttonEvents.drain();
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
      return true;
    }
    // Anything else is dropped: Wi-Fi selection and the manifest load take no presses, and a
    // transfer reads its own Back (backPressedDuringTransfer()).
  }
  return true;
}

// cppcheck-suppress constParameterCallback ; the signature is fui::ListProps::rowProvider's
void FontDownloadActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  const auto* self = static_cast<const FontDownloadActivity*>(ctx);
  const int row = index;
  item.label = "";
  item.actionValue = static_cast<int16_t>(row);
  if (self->isDownloadAllRow(row)) {
    item.label = self->downloadAllLabel_;
    return;
  }
  if (self->isUpdateAllRow(row)) {
    item.label = self->updateAllLabel_;
    return;
  }
  const int familyIndex = self->familyIndexFromList(row);
  if (familyIndex < 0) return;
  const FontCatalog& families = self->families_;
  item.label = families.name(familyIndex);
  const char* description = families.description(familyIndex);
  if (description[0] != '\0') item.subtitle = description;
  if (families.hasUpdate(familyIndex)) {
    item.value = tr(STR_UPDATE_AVAILABLE);
  } else if (families.installed(familyIndex)) {
    item.value = tr(STR_INSTALLED);
  } else if (families.hasResumableDownload(familyIndex)) {
    item.value = tr(STR_RESUME);
  }
}

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);

  // Only the family list lays out here: drawChrome() draws the other states, and families_ is
  // stashed to SD while a transfer runs.
  if (state_ != FAMILY_LIST) return;
  if (families_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  fui::ListProps props = listProps(screen);
  props.labelText = {};  // default label style, not the body-text preset
  props.rowProvider = &FontDownloadActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listItemCount());
  props.valueInset = 8;  // air between the status and the row edge
  addList(screen, props, /*hasSubtitle=*/true);
}

// --- The status screens ---

void FontDownloadActivity::formatSize(const size_t bytes, char* out, const size_t outSize) {
  if (bytes >= 1024 * 1024) {
    snprintf(out, outSize, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(out, outSize, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(out, outSize, "%zu B", bytes);
  }
}

void FontDownloadActivity::drawChrome() {
  UiListActivity::drawChrome();  // the header, through headerTitle(), in every state
  // The family list is buildScreen()'s; Wi-Fi selection shows its own screen on top of this one.
  if (state_ == FAMILY_LIST || state_ == WIFI_SELECTION) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
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
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
  }
}

void FontDownloadActivity::drawFooter() {
  if (state_ == FAMILY_LIST) {
    drawListHints();
    return;
  }
  // Wi-Fi selection and the manifest load take no presses.
  if (state_ == WIFI_SELECTION || state_ == LOADING_MANIFEST) return;

  const char* confirmLabel = "";
  if (state_ == ERROR) {
    // Use the cached value: families_ may have just been restored (post-impl)
    // or still empty (if the failure was in the stash itself); either way the
    // cache reflects the last update from the download attempt.
    const bool canResume = pendingErrorAction_ == PendingFontAction::Download && downloadingFamilyHasResumable_;
    confirmLabel = canResume ? tr(STR_RESUME) : tr(STR_RETRY);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
