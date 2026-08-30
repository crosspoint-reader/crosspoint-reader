#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>

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
  switch (state_) {
    case GROUP_LIST:
      app.clearTapFlash();
      enterGroup(index);
      requestUpdate();
      return;
    case FAMILY_LIST:
      nav.selected = index;
      // Activation starts a download or opens the delete prompt; a lingering
      // flash would gray an unrelated row.
      app.clearTapFlash();
      activateSelected();  // ends with requestUpdateAndWait itself
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return;
  }
}

fui::ListNav& FontDownloadActivity::activeNav() { return state_ == GROUP_LIST ? groupNav_ : nav; }

void FontDownloadActivity::onBackButton() {
  if (state_ != FAMILY_LIST || !hasGroupScreen()) {
    finish();
    return;
  }

  closeRouting();
  {
    RenderLock lock(*this);
    state_ = GROUP_LIST;
    rowsDirty_ = true;
  }
  requestUpdate();
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

  if (!hasGroupScreen()) buildFilteredIndices(0);

  {
    RenderLock lock(*this);
    rowsDirty_ = true;  // families_ just loaded
    if (hasGroupScreen()) {
      groupNav_.reset();
      state_ = GROUP_LIST;
    } else {
      nav.reset();
      state_ = FAMILY_LIST;
    }
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
  scriptGroupLabels_.clear();
  filteredIndices_.clear();
  fontInstaller_.refreshRegistry();

  JsonArray groupsArr = doc["scriptGroups"].as<JsonArray>();
  const size_t groupCount = std::min(groupsArr.size(), MAX_SCRIPT_GROUPS);
  scriptGroupLabels_.reserve(groupCount);
  if (groupsArr.size() > MAX_SCRIPT_GROUPS) {
    LOG_ERR("FONT", "Manifest declares more than %zu script groups; extra groups ignored", MAX_SCRIPT_GROUPS);
  }
  for (size_t groupIndex = 0; groupIndex < groupCount; groupIndex++) {
    JsonObject groupObj = groupsArr[groupIndex].as<JsonObject>();
    const char* tag = groupObj["tag"] | "";
    const char* label = groupObj["label"] | "";
    if (*tag == '\0' || *label == '\0') {
      LOG_ERR("FONT", "Malformed script group at index %zu", groupIndex);
      errorMessage_ = "Invalid font manifest";
      return false;
    }
    scriptGroupLabels_.push_back(label);
  }

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  families_.reserve(familiesArr.size());
  filteredIndices_.reserve(familiesArr.size());

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    family.name = fObj["name"] | "";
    family.description = fObj["description"] | "";

    for (JsonVariant s : fObj["styles"].as<JsonArray>()) {
      family.styles.push_back(s.as<std::string>());
    }

    for (JsonVariant script : fObj["scripts"].as<JsonArray>()) {
      const char* familyTag = script.as<const char*>();
      if (!familyTag) continue;
      for (size_t groupIndex = 0; groupIndex < scriptGroupLabels_.size(); groupIndex++) {
        JsonObject groupObj = groupsArr[groupIndex].as<JsonObject>();
        const char* groupTag = groupObj["tag"] | "";
        if (std::strcmp(familyTag, groupTag) == 0) {
          family.scriptMask |= uint32_t{1} << groupIndex;
          break;
        }
      }
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

  const size_t rowCapacity = std::max(families_.size() + 2, scriptGroupLabels_.size() + 1);
  rowLabels_.reserve(rowCapacity);
  rowItems_.reserve(rowCapacity);

  LOG_DBG("FONT", "Manifest loaded: %zu families, %zu script groups", families_.size(), scriptGroupLabels_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].installed) continue;
    downloadFamily(families_[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].hasUpdate) continue;
    downloadFamily(families_[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].hasUpdate) return true;
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
  return filteredIndices_.empty() ? 0 : static_cast<int>(filteredIndices_.size()) + specialRowCount();
}

int FontDownloadActivity::listCount() const {
  switch (state_) {
    case GROUP_LIST:
      return groupListItemCount();
    case FAMILY_LIST:
      return listItemCount();
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return 0;
  }
  return 0;
}

int FontDownloadActivity::familyIndexFromList(const int listIndex) const {
  const int filteredIndex = listIndex - specialRowCount();
  if (filteredIndex < 0 || filteredIndex >= static_cast<int>(filteredIndices_.size())) return -1;
  return filteredIndices_[filteredIndex];
}

int FontDownloadActivity::groupMemberCount(const int scriptGroupIndex) const {
  if (scriptGroupIndex < 0 || scriptGroupIndex >= static_cast<int>(scriptGroupLabels_.size())) return 0;
  const uint32_t groupBit = uint32_t{1} << scriptGroupIndex;
  int count = 0;
  for (const auto& family : families_) {
    if (family.scriptMask & groupBit) count++;
  }
  return count;
}

void FontDownloadActivity::buildFilteredIndices(const int groupListIndex) {
  filteredIndices_.clear();
  filteredIndices_.reserve(families_.size());
  if (groupListIndex <= 0) {
    for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
      filteredIndices_.push_back(familyIndex);
    }
    return;
  }

  const uint32_t groupBit = uint32_t{1} << (groupListIndex - 1);
  for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
    if (families_[familyIndex].scriptMask & groupBit) filteredIndices_.push_back(familyIndex);
  }
}

void FontDownloadActivity::enterGroup(const int groupListIndex) {
  closeRouting();
  buildFilteredIndices(groupListIndex);
  {
    RenderLock lock(*this);
    nav.reset();
    state_ = FAMILY_LIST;
    rowsDirty_ = true;
  }
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (!families_[familyIndex].installed) total += families_[familyIndex].totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (families_[familyIndex].hasUpdate) total += families_[familyIndex].totalSize;
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

  const int familyIndex = familyIndexFromList(nav.selected);
  if (familyIndex < 0) {
    requestUpdate();
    return;
  }
  auto& family = families_[familyIndex];

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
  if (filteredIndices_.empty()) return;
  if (isDownloadAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (!families_[familyIndex].installed) currentFileTotal_ += families_[familyIndex].files.size();
    }
    downloadAll();
  } else if (isUpdateAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (families_[familyIndex].hasUpdate) currentFileTotal_ += families_[familyIndex].files.size();
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

  if (state_ == FAMILY_LIST && filteredIndices_.empty()) {
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
  syncListViewport(screen, props, /*hasSubtitle=*/state_ == FAMILY_LIST);
  screen.list(props);
}

void FontDownloadActivity::rebuildRowItems() {
  switch (state_) {
    case GROUP_LIST:
      rebuildGroupRowItems();
      return;
    case FAMILY_LIST:
      rebuildFamilyRowItems();
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      rowLabels_.clear();
      rowItems_.clear();
      return;
  }
}

void FontDownloadActivity::rebuildGroupRowItems() {
  const int listSize = groupListItemCount();
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  rowItems_.reserve(listSize);
  for (int rowIndex = 0; rowIndex < listSize; rowIndex++) {
    fui::ListItem item;
    item.label = rowIndex == 0 ? tr(STR_ALL_FONTS) : scriptGroupLabels_[rowIndex - 1].c_str();
    const int memberCount = rowIndex == 0 ? static_cast<int>(families_.size()) : groupMemberCount(rowIndex - 1);
    rowLabels_[rowIndex] = std::to_string(memberCount);
    item.value = rowLabels_[rowIndex].c_str();
    item.actionValue = static_cast<int16_t>(rowIndex);
    rowItems_.push_back(item);
  }
}

void FontDownloadActivity::rebuildFamilyRowItems() {
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
  if (state_ == GROUP_LIST || state_ == FAMILY_LIST) {
    // The base list protocol (Back/Confirm, touch routing, swipe scroll,
    // button navigation) handles both list states.
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

  const char* headerSubtitle = nullptr;
  if (state_ == FAMILY_LIST && hasGroupScreen()) {
    const int scriptGroupIndex = groupNav_.selected - 1;
    headerSubtitle = scriptGroupIndex >= 0 && scriptGroupIndex < static_cast<int>(scriptGroupLabels_.size())
                         ? scriptGroupLabels_[scriptGroupIndex].c_str()
                         : tr(STR_ALL_FONTS);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER),
                 headerSubtitle);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == GROUP_LIST) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == FAMILY_LIST) {
    renderUi();

    const bool hasVisibleFamilies = !filteredIndices_.empty();
    const char* confirmLabel = !hasVisibleFamilies            ? ""
                               : isSelectedFamilyDeletable()  ? tr(STR_DELETE)
                               : isUpdateAllRow(nav.selected) ? tr(STR_UPDATE)
                                                              : tr(STR_DOWNLOAD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, hasVisibleFamilies ? tr(STR_DIR_UP) : "",
                                              hasVisibleFamilies ? tr(STR_DIR_DOWN) : "");
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
