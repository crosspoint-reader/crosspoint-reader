#include "ThemeDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

ThemeDownloadActivity::ThemeDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ThemeDownload", renderer, mappedInput), themeInstaller_(UITheme::getInstance().registry()) {}

void ThemeDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ThemeDownloadActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

void ThemeDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
    downloadingThemeIndex_ = -1;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  {
    RenderLock lock(*this);
    state_ = THEME_LIST;
    selectedIndex_ = 0;
  }
}

bool ThemeDownloadActivity::fetchAndParseManifest() {
  static constexpr const char* MANIFEST_TMP = "/themes_manifest.tmp";

  auto result = HttpDownloader::downloadToFile(THEME_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("THEME", "Failed to fetch manifest from %s", THEME_MANIFEST_URL);
    errorMessage_ = "Failed to fetch theme list";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  HalFile manifestFile;
  if (!Storage.openFileForRead("THEME", MANIFEST_TMP, manifestFile)) {
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read theme list";
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (err) {
    LOG_ERR("THEME", "Manifest parse error: %s", err.c_str());
    errorMessage_ = "Invalid theme manifest";
    return false;
  }

  const int version = doc["version"] | 0;
  if (version != THEMES_MANIFEST_VERSION) {
    LOG_ERR("THEME", "Unsupported manifest version: %d", version);
    errorMessage_ = "Unsupported manifest version";
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  static constexpr const char* HTTPS_THEME_ASSETS = "https://crosspointreader.com/themes/";
  static constexpr const char* HTTP_THEME_ASSETS = "http://crosspointreader.com/themes/";
  if (baseUrl_.rfind(HTTPS_THEME_ASSETS, 0) == 0) {
    baseUrl_.replace(0, strlen(HTTPS_THEME_ASSETS), HTTP_THEME_ASSETS);
  }
  themes_.clear();
  themeInstaller_.refreshRegistry();

  JsonArray themesArr = doc["themes"].as<JsonArray>();
  themes_.reserve(themesArr.size());

  for (JsonObject tObj : themesArr) {
    ManifestTheme theme;
    theme.id = tObj["id"] | "";
    theme.name = tObj["name"] | theme.id;
    theme.description = tObj["description"] | "";

    if (!ThemeInstaller::isValidThemeId(theme.id.c_str())) {
      errorMessage_ = "Invalid theme manifest";
      return false;
    }

    for (JsonObject fileObj : tObj["files"].as<JsonArray>()) {
      ManifestFile file;
      file.path = fileObj["path"] | fileObj["name"] | "";
      file.url = fileObj["url"] | file.path;
      file.size = fileObj["size"] | 0;
      if (!ThemeInstaller::isValidRelativePath(file.path.c_str()) || !ThemeInstaller::isValidRelativePath(file.url.c_str()) ||
          !fileObj["crc32"].is<uint32_t>()) {
        errorMessage_ = "Invalid theme manifest";
        return false;
      }
      file.crc32 = fileObj["crc32"].as<uint32_t>();
      theme.totalSize += file.size;
      theme.files.push_back(std::move(file));
    }

    theme.installed = themeInstaller_.isThemeInstalled(theme.id.c_str());
    if (theme.installed) {
      for (const auto& file : theme.files) {
        char path[180];
        ThemeInstaller::buildThemePath(theme.id.c_str(), file.path.c_str(), path, sizeof(path));
        HalFile f;
        if (Storage.openFileForRead("THEME", path, f)) {
          const size_t actual = f.fileSize();
          f.close();
          if (actual != file.size) {
            theme.hasUpdate = true;
            break;
          }
        } else {
          theme.hasUpdate = true;
          break;
        }
      }
    }

    themes_.push_back(std::move(theme));
  }

  LOG_DBG("THEME", "Manifest loaded: %zu themes", themes_.size());
  return true;
}

void ThemeDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (auto& theme : themes_) {
    if (theme.installed) continue;
    downloadTheme(theme);
    if (state_ == ERROR || cancelRequested_) return;
  }
  RenderLock lock(*this);
  state_ = COMPLETE;
}

void ThemeDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (auto& theme : themes_) {
    if (!theme.hasUpdate) continue;
    downloadTheme(theme);
    if (state_ == ERROR || cancelRequested_) return;
  }
  RenderLock lock(*this);
  state_ = COMPLETE;
}

bool ThemeDownloadActivity::showDownloadAllRow() const {
  for (const auto& t : themes_) {
    if (!t.installed) return true;
  }
  return false;
}

bool ThemeDownloadActivity::showUpdateAllRow() const {
  for (const auto& t : themes_) {
    if (t.hasUpdate) return true;
  }
  return false;
}

int ThemeDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool ThemeDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool ThemeDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int ThemeDownloadActivity::listItemCount() const {
  return themes_.empty() ? 0 : static_cast<int>(themes_.size()) + specialRowCount();
}

size_t ThemeDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& t : themes_) {
    if (!t.installed) total += t.totalSize;
  }
  return total;
}

size_t ThemeDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& t : themes_) {
    if (t.hasUpdate) total += t.totalSize;
  }
  return total;
}

bool ThemeDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("THEME", path, f)) return false;
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

void ThemeDownloadActivity::downloadTheme(ManifestTheme& theme) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingThemeIndex_ = static_cast<int>(&theme - themes_.data());
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  if (!themeInstaller_.ensureThemeDir(theme.id.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create theme directory";
    return;
  }

  for (size_t i = 0; i < theme.files.size(); i++) {
    const auto& file = theme.files[i];
    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[180];
    ThemeInstaller::buildThemePath(theme.id.c_str(), file.path.c_str(), destPath, sizeof(destPath));
    if (!themeInstaller_.ensureParentDirs(destPath)) {
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Failed to create theme directory";
      return;
    }

    std::string url = baseUrl_ + file.url;
    auto result = HttpDownloader::downloadToFile(
        url, destPath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          requestUpdate(true);
        },
        &cancelRequested_);

    if (result == HttpDownloader::ABORTED) {
      themeInstaller_.deleteTheme(theme.id.c_str());
      theme.installed = false;
      theme.hasUpdate = false;
      RenderLock lock(*this);
      state_ = THEME_LIST;
      return;
    }

    if (result != HttpDownloader::OK) {
      themeInstaller_.deleteTheme(theme.id.c_str());
      theme.installed = false;
      theme.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Download failed: " + file.path;
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(destPath, actualCrc) || actualCrc != file.crc32 || !themeInstaller_.validateThemeFile(destPath)) {
      themeInstaller_.deleteTheme(theme.id.c_str());
      theme.installed = false;
      theme.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Invalid theme file: " + file.path;
      return;
    }
    currentFileIndex_++;
  }

  themeInstaller_.refreshRegistry();
  theme.installed = true;
  theme.hasUpdate = false;
  RenderLock lock(*this);
  state_ = COMPLETE;
}

void ThemeDownloadActivity::promptDeleteSelectedTheme() {
  const int pendingDeleteThemeIndex = themeIndexFromList(selectedIndex_);
  if (pendingDeleteThemeIndex < 0 || pendingDeleteThemeIndex >= static_cast<int>(themes_.size())) return;

  const auto& theme = themes_[pendingDeleteThemeIndex];
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), theme.name),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void ThemeDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  auto& theme = themes_[themeIndexFromList(selectedIndex_)];
  if (themeInstaller_.deleteTheme(theme.id.c_str()) != ThemeInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to delete theme";
  } else {
    themeInstaller_.refreshRegistry();
    theme.installed = false;
    theme.hasUpdate = false;
  }
  requestUpdate();
}

bool ThemeDownloadActivity::isSelectedThemeDeletable() const {
  if (isDownloadAllRow(selectedIndex_) || isUpdateAllRow(selectedIndex_)) return false;
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  const auto& theme = themes_[themeIndexFromList(selectedIndex_)];
  return theme.installed && !theme.hasUpdate;
}

void ThemeDownloadActivity::loop() {
  if (state_ == THEME_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
    buttonNavigator_.onNextRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });
    buttonNavigator_.onPreviousRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });
    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });
    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !themes_.empty()) {
      if (isDownloadAllRow(selectedIndex_)) {
        currentFileIndex_ = 0;
        currentFileTotal_ = 0;
        for (const auto& t : themes_) {
          if (!t.installed) currentFileTotal_ += t.files.size();
        }
        downloadAll();
      } else if (isUpdateAllRow(selectedIndex_)) {
        currentFileIndex_ = 0;
        currentFileTotal_ = 0;
        for (const auto& t : themes_) {
          if (t.hasUpdate) currentFileTotal_ += t.files.size();
        }
        updateAll();
      } else {
        auto& theme = themes_[themeIndexFromList(selectedIndex_)];
        if (!theme.installed || theme.hasUpdate) {
          currentFileIndex_ = 0;
          currentFileTotal_ = theme.files.size();
          downloadTheme(theme);
        } else {
          promptDeleteSelectedTheme();
          return;
        }
      }
      requestUpdateAndWait();
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      RenderLock lock(*this);
      state_ = THEME_LIST;
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      RenderLock lock(*this);
      state_ = THEME_LIST;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingThemeIndex_ >= 0 && downloadingThemeIndex_ < static_cast<int>(themes_.size())) {
        downloadTheme(themes_[downloadingThemeIndex_]);
        requestUpdateAndWait();
      } else {
        {
          RenderLock lock(*this);
          state_ = LOADING_MANIFEST;
          errorMessage_.clear();
        }
        requestUpdateAndWait();

        if (!fetchAndParseManifest()) {
          RenderLock lock(*this);
          state_ = ERROR;
        } else {
          RenderLock lock(*this);
          state_ = THEME_LIST;
          selectedIndex_ = 0;
        }
        requestUpdate();
      }
    }
  }
}

std::string ThemeDownloadActivity::formatSize(size_t bytes) {
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

void ThemeDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_THEME_BROWSER));
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_THEME_LIST));
  } else if (state_ == THEME_LIST) {
    if (themes_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_THEMES_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index)) return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
            if (isUpdateAllRow(index)) return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            return themes_[themeIndexFromList(index)].name;
          },
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            return themes_[themeIndexFromList(index)].description;
          },
          nullptr,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            const auto& t = themes_[themeIndexFromList(index)];
            if (t.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (t.installed) return tr(STR_INSTALLED);
            return "";
          },
          true,
          [this](int index) -> bool {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return false;
            const auto& t = themes_[themeIndexFromList(index)];
            return t.installed && !t.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                isSelectedThemeDeletable()     ? tr(STR_DELETE)
                                                : isUpdateAllRow(selectedIndex_) ? tr(STR_UPDATE)
                                                                                 : tr(STR_DOWNLOAD),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const auto& theme = themes_[downloadingThemeIndex_];
    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + theme.name + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, centerY + metrics.verticalSpacing, pageWidth - metrics.contentSidePadding * 2,
             metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_THEME_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_THEME_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
