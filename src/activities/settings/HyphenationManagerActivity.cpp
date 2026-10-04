#include "HyphenationManagerActivity.h"

#include <ArduinoJson.h>
#include <Epub/hyphenation/LanguageRegistry.h>
#include <FontCacheManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "HyphenationPackStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;
namespace {
constexpr char MANIFEST_URL[] =
    "https://github.com/crosspoint-reader/crosspoint-assets/releases/latest/download/hyphenation.json";
constexpr char MANIFEST_TMP[] = "/.crosspoint/hyphenation_manifest.tmp";
constexpr char PACK_TMP[] = "/.crosspoint/hyphenation_pack.tmp";
constexpr size_t MAX_MANIFEST_SIZE = 32 * 1024;
constexpr size_t MAX_JSON_MEMORY = 24 * 1024;

// ArduinoJson owns these blocks; account for them before allowing DOM growth.
class ManifestAllocator final : public ArduinoJson::Allocator {
  struct alignas(std::max_align_t) Block {
    size_t size;
  };
  size_t used_ = 0;

 public:
  void* allocate(size_t size) override {
    if (size > MAX_JSON_MEMORY - used_ || sizeof(Block) > MAX_JSON_MEMORY - used_ - size) return nullptr;
    auto* block = static_cast<Block*>(std::malloc(sizeof(Block) + size));
    if (!block) return nullptr;
    block->size = sizeof(Block) + size;
    used_ += block->size;
    return block + 1;
  }

  void deallocate(void* ptr) override {
    if (!ptr) return;
    auto* block = static_cast<Block*>(ptr) - 1;
    used_ -= block->size;
    std::free(block);
  }

  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    const size_t oldSize = (static_cast<Block*>(ptr) - 1)->size - sizeof(Block);
    if (size <= oldSize) return ptr;
    // Both blocks count against the budget while a growing string is copied.
    void* next = allocate(size);
    if (!next) return nullptr;
    std::memcpy(next, ptr, oldSize);
    deallocate(ptr);
    return next;
  }
};

bool isPackFilename(const char* name) {
  return std::strlen(name) == 14 && std::strncmp(name, "hyph-", 5) == 0 && name[5] >= 'a' && name[5] <= 'z' &&
         name[6] >= 'a' && name[6] <= 'z' && std::strcmp(name + 7, ".cphyph") == 0;
}
}  // namespace

HyphenationManagerActivity::HyphenationManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const char* wantedCode)
    : UiListActivity("HyphenationManager", renderer, mappedInput) {
  if (wantedCode && wantedCode[0] && wantedCode[1] && !wantedCode[2]) {
    wantedCode_[0] = wantedCode[0];
    wantedCode_[1] = wantedCode[1];
  }
}

void HyphenationManagerActivity::loadOfflineList() {
  packCount_ = 0;
  for (const LanguageEntry& language : getLanguageEntries()) {
    Pack& pack = packs_[packCount_++];
    std::snprintf(pack.code, sizeof(pack.code), "%s", language.primaryTag);
    std::snprintf(pack.name, sizeof(pack.name), "%s", language.cliName);
    if (pack.name[0] >= 'a' && pack.name[0] <= 'z') pack.name[0] -= 'a' - 'A';
    pack.size = 0;
    pack.fileCrc = 0;
    pack.payloadCrc = 0;
    pack.supported = true;
    pack.builtIn = language.hyphenator != nullptr;
  }
}

void HyphenationManagerActivity::scanSdRoot() {
  HalFile root = Storage.open("/");
  if (!root || !root.isDirectory()) {
    LOG_ERR("HYPH", "Could not scan SD root");
    return;
  }
  for (HalFile item = root.openNextFile(); item; item = root.openNextFile()) {
    char name[40];
    item.getName(name, sizeof(name));
    const bool candidate = !item.isDirectory() && isPackFilename(name);
    item.close();
    if (!candidate) continue;
    char path[42];
    std::snprintf(path, sizeof(path), "/%s", name);
    const auto result = HyphenationPackStore::installFromSd(path);
    if (result == HyphenationPackStore::InstallResult::OK) {
      if (!Storage.remove(path)) LOG_ERR("HYPH", "Could not remove imported %s", path);
    } else {
      LOG_ERR("HYPH", "Could not import %s: %u", path, static_cast<unsigned>(result));
      failedCode_[0] = name[5];
      failedCode_[1] = name[6];
    }
  }
}

void HyphenationManagerActivity::onEnter() {
  UiListActivity::onEnter();
  rows_ = makeUniqueNoThrow<fui::ListItem[]>(MAX_PACKS);
  if (!rows_) {
    LOG_ERR("HYPH", "OOM: manager rows");
    finish();
    return;
  }
  loadOfflineList();
  scanSdRoot();
  if (wantedCode_[0] && HyphenationPackStore::isInstalled(wantedCode_)) {
    finish();
    return;
  }
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  WiFi.mode(WIFI_STA);
  wifiStarted_ = true;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("HYPH", "OOM: Wi-Fi selection");
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
}

void HyphenationManagerActivity::onExit() {
  UiListActivity::onExit();
  if (wifiStarted_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (wantedCode_[0]) {
      if (HyphenationPackStore::isInstalled(wantedCode_)) {
        silentRestartToReader();
      } else {
        WiFi.mode(WIFI_OFF);
      }
    } else {
      silentRestartToSettings();
    }
  }
}

bool HyphenationManagerActivity::loadManifest() {
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("HYPH", "Low heap for language list");
    return false;
  }
  Storage.remove(MANIFEST_TMP);
  if (HttpDownloader::downloadToFile(MANIFEST_URL, MANIFEST_TMP, nullptr) != HttpDownloader::OK) {
    LOG_ERR("HYPH", "Could not download language list");
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("HYPH", MANIFEST_TMP, file)) {
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  if (file.size() > MAX_MANIFEST_SIZE) {
    LOG_ERR("HYPH", "Language list exceeds %zu bytes", MAX_MANIFEST_SIZE);
    file.close();
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  ManifestAllocator allocator;
  JsonDocument document(&allocator);
  JsonDocument filter(&allocator);
  filter["version"] = true;
  filter["baseUrl"] = true;
  for (const char* key : {"code", "name", "file", "size", "crc32", "payloadCrc32"}) filter["packs"][0][key] = true;
  if (filter.overflowed()) {
    LOG_ERR("HYPH", "OOM: language list filter");
    file.close();
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  DeserializationError error = deserializeJson(document, file, DeserializationOption::Filter(filter));
  file.close();
  Storage.remove(MANIFEST_TMP);
  if (error) {
    LOG_ERR("HYPH", "Language list parse error: %s", error.c_str());
    return false;
  }
  if ((document["version"] | 0) != 1) return false;
  JsonArray entries = document["packs"].as<JsonArray>();
  if (entries.isNull() || entries.size() == 0) return false;
  const char* base = document["baseUrl"] | "";
  if (std::strncmp(base, "https://", 8) != 0 || std::strlen(base) > 180) return false;
  baseUrl_ = base;
  if (baseUrl_.back() != '/') return false;

  size_t count = 0;
  // Supported languages get capacity first; retain other rows as disabled when they fit.
  for (const bool supported : {true, false}) {
    for (JsonObject item : entries) {
      const char* code = item["code"] | "";
      const LanguageEntry* language = findLanguageEntry(code);
      if ((language != nullptr) != supported || count == MAX_PACKS) continue;
      const char* name = item["name"] | "";
      const char* filename = item["file"] | "";
      char expected[16];
      if (std::strlen(code) != 2 || code[0] < 'a' || code[0] > 'z' || code[1] < 'a' || code[1] > 'z') return false;
      std::snprintf(expected, sizeof(expected), "hyph-%s.cphyph", code);
      if (std::strcmp(filename, expected) != 0 || !name[0] || std::strlen(name) >= sizeof(Pack::name) ||
          !item["size"].is<uint32_t>() || !item["crc32"].is<uint32_t>() || !item["payloadCrc32"].is<uint32_t>()) {
        return false;
      }
      for (size_t j = 0; j < count; ++j) {
        if (std::strcmp(packs_[j].code, code) == 0) return false;
      }
      Pack& pack = packs_[count++];
      std::snprintf(pack.code, sizeof(pack.code), "%s", code);
      std::snprintf(pack.name, sizeof(pack.name), "%s", name);
      pack.size = item["size"].as<uint32_t>();
      pack.fileCrc = item["crc32"].as<uint32_t>();
      pack.payloadCrc = item["payloadCrc32"].as<uint32_t>();
      pack.supported = language != nullptr;
      pack.builtIn = language && language->hyphenator;
    }
    if (supported && count == 0) return false;
  }
  packCount_ = count;
  return true;
}

void HyphenationManagerActivity::onWifiReady(const bool connected) {
  if (!connected) {
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    loading_ = true;
  }
  requestUpdateAndWait();
  if (!loadManifest()) {
    LOG_ERR("HYPH", "Invalid language list");
    manifestFailed_ = true;
    baseUrl_.clear();
    loadOfflineList();
  }
  {
    RenderLock lock(*this);
    loading_ = false;
    nav.reset();
  }
  requestUpdate();
}

bool HyphenationManagerActivity::fileCrcMatches(const char* path, const uint32_t expected) {
  HalFile file;
  if (!Storage.openFileForRead("HYPH", path, file)) return false;
  uint8_t buffer[128];
  uint32_t crc = 0;
  while (file.available()) {
    const int n = file.read(buffer, sizeof(buffer));
    if (n <= 0) return false;
    crc = esp_rom_crc32_le(crc, buffer, n);
  }
  return crc == expected;
}

bool HyphenationManagerActivity::downloadPack(Pack& pack) {
  if (baseUrl_.empty() || !pack.size) return false;
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("HYPH", "Low heap for pack download");
    return false;
  }
  {
    RenderLock lock(*this);
    loading_ = true;
  }
  requestUpdateAndWait();
  char filename[16];
  std::snprintf(filename, sizeof(filename), "hyph-%s.cphyph", pack.code);
  const std::string url = baseUrl_ + filename;
  Storage.remove(PACK_TMP);
  const auto result = HttpDownloader::downloadToFile(url, PACK_TMP, nullptr, nullptr, "", "", {}, true);
  bool installed = false;
  if (result == HttpDownloader::OK) {
    HalFile file;
    if (Storage.openFileForRead("HYPH", PACK_TMP, file)) {
      const bool expectedSize = file.size() == pack.size;
      file.close();
      if (expectedSize && fileCrcMatches(PACK_TMP, pack.fileCrc)) {
        installed = HyphenationPackStore::installFromSd(PACK_TMP) == HyphenationPackStore::InstallResult::OK;
      }
    }
  }
  Storage.remove(PACK_TMP);
  if (!installed) LOG_ERR("HYPH", "Could not install %s", filename);
  failedCode_[0] = installed ? '\0' : pack.code[0];
  failedCode_[1] = installed ? '\0' : pack.code[1];
  {
    RenderLock lock(*this);
    loading_ = false;
  }
  requestUpdate();
  return installed;
}

void HyphenationManagerActivity::confirmRemoval(const Pack& pack) {
  char code[3] = {pack.code[0], pack.code[1], '\0'};
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_HYPHENATION_REMOVE), pack.name);
  if (!dialog) {
    LOG_ERR("HYPH", "OOM: remove confirmation");
    return;
  }
  startActivityForResult(std::move(dialog), [this, code0 = code[0], code1 = code[1]](const ActivityResult& result) {
    if (result.isCancelled) return;
    const char selected[3] = {code0, code1, '\0'};
    if (!HyphenationPackStore::remove(selected)) LOG_ERR("HYPH", "Could not remove %s", selected);
  });
}

void HyphenationManagerActivity::chooseUpdateOrRemoval(Pack& pack) {
  const char code0 = pack.code[0];
  const char code1 = pack.code[1];
  auto choice = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, pack.name, tr(STR_HYPHENATION_UPDATE_PROMPT), StrId::STR_UPDATE, StrId::STR_DELETE);
  if (!choice) {
    LOG_ERR("HYPH", "OOM: pack action choice");
    return;
  }
  startActivityForResult(std::move(choice), [this, code0, code1](const ActivityResult& result) {
    const auto* selected = std::get_if<MenuResult>(&result.data);
    if (!selected) return;
    const char code[3] = {code0, code1, '\0'};
    for (size_t i = 0; i < packCount_; ++i) {
      if (std::strcmp(packs_[i].code, code) != 0) continue;
      if (selected->action == 0) downloadPack(packs_[i]);
      if (selected->action == 1) confirmRemoval(packs_[i]);
      return;
    }
  });
}

void HyphenationManagerActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(packCount_)) return;
  Pack& pack = packs_[index];
  if (!pack.supported || pack.builtIn) return;
  ExternalHyphenationPatterns installed{};
  const bool hasPack = HyphenationPackStore::lookup(pack.code, installed);
  if (hasPack && pack.payloadCrc && installed.identity != pack.payloadCrc && !baseUrl_.empty()) {
    chooseUpdateOrRemoval(pack);
  } else if (hasPack) {
    confirmRemoval(pack);
  } else if (!baseUrl_.empty()) {
    downloadPack(pack);
  }
}

const char* HyphenationManagerActivity::headerTitle() const { return tr(STR_MANAGE_HYPHENATION); }

void HyphenationManagerActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, headerTitle(),
                 loading_           ? nullptr
                 : manifestFailed_  ? tr(STR_HYPHENATION_LIST_FAILED)
                 : baseUrl_.empty() ? tr(STR_HYPHENATION_OFFLINE)
                                    : nullptr);
}

void HyphenationManagerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (loading_) {
    screen.centeredText(tr(STR_HYPHENATION_LOADING), screen.theme().bodyText);
    return;
  }
  for (size_t i = 0; i < packCount_; ++i) {
    const Pack& pack = packs_[i];
    fui::ListItem& row = rows_[i];
    row = {};
    row.label = pack.name;
    row.actionValue = static_cast<int16_t>(i);
    if (pack.builtIn) {
      row.value = tr(STR_HYPHENATION_BUILTIN);
    } else if (!pack.supported) {
      row.value = tr(STR_HYPHENATION_UNSUPPORTED);
      row.state = fui::StateDisabled;
    } else if (pack.code[0] == failedCode_[0] && pack.code[1] == failedCode_[1]) {
      row.value = tr(STR_HYPHENATION_INSTALL_FAILED);
    } else {
      ExternalHyphenationPatterns existing{};
      if (HyphenationPackStore::lookup(pack.code, existing)) {
        row.value =
            pack.payloadCrc && existing.identity != pack.payloadCrc ? tr(STR_UPDATE_AVAILABLE) : tr(STR_INSTALLED);
      } else {
        row.value = tr(STR_HYPHENATION_NOT_INSTALLED);
      }
    }
  }
  fui::ListProps props;
  props.items = rows_.get();
  props.count = static_cast<uint16_t>(packCount_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}
