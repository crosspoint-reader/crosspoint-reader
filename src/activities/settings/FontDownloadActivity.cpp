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

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
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
  if (version != 1) {
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

    for (JsonVariant s : fObj["styles"].as<JsonArray>()) {
      family.styles.push_back(s.as<std::string>());
    }

    family.totalSize = 0;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
      ManifestFile file;
      file.name = fileObj["name"] | "";
      file.size = fileObj["size"] | 0;
      family.totalSize += file.size;
      family.files.push_back(std::move(file));
    }

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
    }

    families_.push_back(std::move(family));
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].installed || !families_[i].hasUpdate) continue;
    downloadFamily(families_[i]);
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

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  cancelRequested_ = false;
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = family.files.size();
    fileProgress_ = 0;
    fileTotal_ = 0;
  }
  requestUpdateAndWait();

  char liveDir[128];
  char stagingDir[128];
  char backupDir[128];
  snprintf(liveDir, sizeof(liveDir), "%s/%s", SdCardFontRegistry::FONTS_DIR, family.name.c_str());
  snprintf(stagingDir, sizeof(stagingDir), "%s/%s__staging", SdCardFontRegistry::FONTS_DIR, family.name.c_str());
  snprintf(backupDir, sizeof(backupDir), "%s/%s__backup", SdCardFontRegistry::FONTS_DIR, family.name.c_str());

  if (Storage.exists(stagingDir) && !Storage.removeDir(stagingDir)) {
    LOG_ERR("FONT", "Failed to clean staging dir: %s", stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    errorMessage_ = "Failed to prepare staging area";
    return;
  }

  if (!Storage.mkdir(stagingDir)) {
    LOG_ERR("FONT", "Failed to create staging dir: %s", stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
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

    // Make sure parent directories exist for the file
    std::string stagedPathStr(stagedPath);
    size_t lastSlash = stagedPathStr.find_last_of('/');
    if (lastSlash != std::string::npos) {
      Storage.mkdir(stagedPathStr.substr(0, lastSlash).c_str());
    }

    std::string url = baseUrl_ + file.name;

    auto result = HttpDownloader::downloadToFile(url, stagedPath, [this](unsigned int downloaded, unsigned int total) {
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
      Storage.removeDir(stagingDir);
      cancelRequested_ = true;
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      Storage.removeDir(stagingDir);
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
      errorMessage_ = "Download failed: " + file.name;
      return;
    }

    if (!fontInstaller_.validateCpfontFile(stagedPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", stagedPath);
      Storage.removeDir(stagingDir);
      RenderLock lock(*this);
      state_ = ERROR;
      pendingErrorAction_ = PendingFontAction::Download;
      downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
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
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    errorMessage_ = "Failed to prepare backup area";
    return;
  }

  if (hadLiveDir && !Storage.rename(liveDir, backupDir)) {
    LOG_ERR("FONT", "Failed to move live family to backup: %s", liveDir);
    Storage.removeDir(stagingDir);
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
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
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    errorMessage_ = "Failed to finalize font install";
    return;
  }

  if (Storage.exists(backupDir) && !Storage.removeDir(backupDir)) {
    LOG_INF("FONT", "Failed to remove backup dir after successful install: %s", backupDir);
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;
  syncSelectedIndexForNewActionCount();

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
            downloadFamily(families_[familyIndex]);
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
          downloadFamily(families_[downloadingFamilyIndex_]);
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
            return "";
          },
          true);

      const std::string confirmLabel = confirmButtonLabel();
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel.c_str(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + family.name + " (" +
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
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
