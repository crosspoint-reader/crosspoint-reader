#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

void FontDownloadActivity::activateIndex(const int index) {
  if (state_ != FAMILY_LIST) return;
  nav.selected = index;
  // Activation starts a download or opens the delete prompt; a lingering
  // flash would gray an unrelated row.
  app.clearTapFlash();
  activateSelected();  // ends with requestUpdateAndWait itself
}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();
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
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    rowsDirty_ = true;  // families_ just loaded
    nav.selected = 0;
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // Download manifest to a temp file on SD card to avoid holding both
  // TLS buffers and the full JSON string in RAM simultaneously.
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  // HTTP client is now closed — TLS buffers freed. Parse JSON from file.
  HalFile manifestFile;
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
  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = "Unsupported manifest version";
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  families_.clear();
  fontInstaller_.refreshRegistry();

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

      if (!fileObj["crc32"].is<uint32_t>()) {
        LOG_ERR("FONT", "Malformed manifest file entry: missing or invalid crc32 for %s", file.name.c_str());
        errorMessage_ = "Invalid font manifest";
        return false;
      }
      file.crc32 = fileObj["crc32"].as<uint32_t>();

      family.totalSize += file.size;
      family.files.push_back(std::move(file));
    }

    family.installed = fontInstaller_.isFamilyInstalled(family.name.c_str());

    // Detect updates by comparing manifest file sizes with files on disk.
    // Not a checksum, but a size mismatch reliably indicates a rebuild in practice.
    if (family.installed) {
      for (const auto& file : family.files) {
        char path[128];
        FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), path, sizeof(path));
        HalFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != file.size) {
            family.hasUpdate = true;
            break;
          }
        } else {
          // File missing on disk but family dir exists — treat as update
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

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].hasUpdate) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const auto& f : families_) {
    if (!f.installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const auto& f : families_) {
    if (f.hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return families_.empty() ? 0 : static_cast<int>(families_.size()) + specialRowCount();
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed) total += f.totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.hasUpdate) total += f.totalSize;
  }
  return total;
}

// Standard CRC32 matching zlib/Python zlib.crc32().
bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
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

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
  }
  requestUpdateAndWait();

  if (!fontInstaller_.ensureFamilyDir(family.name.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[128];
    FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), destPath, sizeof(destPath));

    std::string url = baseUrl_ + file.name;

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
          // This update() consumes the one-shot home event before the central
          // ActivityManager dispatch can see it, so honor it here: abort the
          // download, then exit to home once the abort unwinds.
          if (mappedInput.wasHomeGesture()) {
            cancelRequested_ = true;
            goHomeRequested_ = true;
          }
          requestUpdate(true);
        },
        &cancelRequested_);

    if (result == HttpDownloader::ABORTED) {
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      if (goHomeRequested_) {
        onGoHome();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // installed/hasUpdate just changed above
      }
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Download failed: " + file.name;
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(destPath, actualCrc)) {
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", destPath);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Failed to compute checksum: " + file.name;
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Checksum mismatch: " + file.name;
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(destPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", destPath);
      fontInstaller_.deleteFamily(family.name.c_str());
      family.installed = false;
      family.hasUpdate = false;
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Invalid font file: " + file.name;
      return;
    }
    currentFileIndex_++;
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(nav.selected);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = family.name;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  auto& family = families_[familyIndexFromList(nav.selected)];

  if (fontInstaller_.deleteFamily(family.name.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to delete font";
  } else {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    // Unlike the other family_ mutations, this one stays in FAMILY_LIST (no
    // state_ transition to hang the rebuild off), so it must set the flag
    // directly.
    rowsDirty_ = true;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(nav.selected) || isUpdateAllRow(nav.selected)) return false;
  if (nav.selected < specialRowCount() || nav.selected >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(nav.selected)];
  return family.installed && !family.hasUpdate;
}

void FontDownloadActivity::activateSelected() {
  if (families_.empty()) return;
  if (isDownloadAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const auto& f : families_) {
      if (!f.installed) currentFileTotal_ += f.files.size();
    }
    downloadAll();
  } else if (isUpdateAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const auto& f : families_) {
      if (f.hasUpdate) currentFileTotal_ += f.files.size();
    }
    updateAll();
  } else {
    // The special rows disappear when a download starts, so a stale selection
    // can map past the family table.
    const int familyIndex = familyIndexFromList(nav.selected);
    if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
    auto& family = families_[familyIndex];
    if (!family.installed || family.hasUpdate) {
      currentFileIndex_ = 0;
      currentFileTotal_ = family.files.size();
      downloadFamily(family);
    } else {
      promptDeleteSelectedFamily();
      return;
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (families_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  if (rowsDirty_) {
    rebuildRowItems();
    rowsDirty_ = false;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the status and the row edge
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

// Rebuilds rowLabels_/rowItems_ from families_. Only called when rowsDirty_ is
// set (families_/state_ changed since the last build), never on every repaint.
void FontDownloadActivity::rebuildRowItems() {
  const int listSize = listItemCount();
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  rowItems_.reserve(listSize);
  for (int i = 0; i < listSize; i++) {
    fui::ListItem item;
    if (isDownloadAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else if (isUpdateAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else {
      const auto& family = families_[familyIndexFromList(i)];
      item.label = family.name.c_str();
      if (!family.description.empty()) item.subtitle = family.description.c_str();
      if (family.hasUpdate) {
        item.value = tr(STR_UPDATE_AVAILABLE);
      } else if (family.installed) {
        item.value = tr(STR_INSTALLED);
        // Dimmed but still tappable (opens the delete prompt): visual-only
        // disabled state, the row stays enabled for hit registration.
        item.state = fui::StateDisabled;
      }
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

// --- Input handling ---

bool FontDownloadActivity::handleCustomInput() {
  if (state_ == FAMILY_LIST) {
    // The base list protocol (Back/Confirm, touch routing, swipe scroll,
    // button navigation) handles this state.
    return false;
  }

  if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the completed download changed installed/hasUpdate
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the failed download reset installed/hasUpdate
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        downloadFamily(families_[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return true;
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
          downloadFamily(families_[downloadingFamilyIndex_]);
          requestUpdateAndWait();
          return true;
        }
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    }
  }

  return true;
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    renderUi();

    const char* confirmLabel = families_.empty()              ? ""
                               : isSelectedFamilyDeletable()  ? tr(STR_DELETE)
                               : isUpdateAllRow(nav.selected) ? tr(STR_UPDATE)
                                                              : tr(STR_DOWNLOAD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, families_.empty() ? "" : tr(STR_DIR_UP),
                                              families_.empty() ? "" : tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
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
