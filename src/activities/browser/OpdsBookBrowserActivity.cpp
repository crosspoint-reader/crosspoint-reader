#include "OpdsBookBrowserActivity.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <SaxParser/SaxParser.h>
#include <WiFi.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsFormatLabel.h"
#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/NetworkMode.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/themes/ListTouchBand.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/OpdsFilename.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
// The catalog draws CATALOG_ROW_HEIGHT rows from CATALOG_LIST_TOP down to the foot of the content
// area; catalogRowsPerPage() turns that into the page both the render and paging use.
constexpr int CATALOG_LIST_TOP = 60;
constexpr int CATALOG_ROW_HEIGHT = 30;
// Entries reserved before a fetch: about one portrait page, plus the prev/next navigation entries.
constexpr size_t FEED_RESERVE_ENTRIES = 25;

// Hard ceiling on entries held from a single feed. Each entry costs 4 bytes in
// the in-RAM entryOffsets index; the bodies live on the SD cache file. Well-behaved
// OPDS servers paginate (we follow the next/prev links), so this only bites on a
// server returning one giant unpaginated feed — where we'd otherwise grow the
// offset vector until the heap OOMs. 2000 entries ≈ 8 KB, a safe bound on 380 KB RAM.
constexpr size_t MAX_FEED_ENTRIES = 2000;
constexpr int FORMAT_ITEM_HEIGHT = 30;
constexpr int FORMAT_LIST_TOP_OFFSET = 20;
constexpr int FORMAT_LIST_BOTTOM_PADDING = 20;

int formatItemsPerPage(const Rect& contentRect) {
  const int midY = contentRect.y + contentRect.height / 2;
  const int listTop = midY + FORMAT_LIST_TOP_OFFSET;
  const int listBottom = contentRect.y + contentRect.height - FORMAT_LIST_BOTTOM_PADDING;
  const int rawAvailableHeight = listBottom - listTop;
  const int availableHeight = rawAvailableHeight > FORMAT_ITEM_HEIGHT ? rawAvailableHeight : FORMAT_ITEM_HEIGHT;
  const int itemsPerPage = availableHeight / FORMAT_ITEM_HEIGHT;
  return itemsPerPage > 0 ? itemsPerPage : 1;
}

void writeString(HalFile& f, const std::string& s) {
  if (s.length() > UINT16_MAX) {
    LOG_ERR("OPDS", "writeString: string truncated from %zu to %u bytes", s.length(), UINT16_MAX);
  }
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.length(), UINT16_MAX));
  f.write(reinterpret_cast<const uint8_t*>(&len), sizeof(len));
  if (len > 0) f.write(reinterpret_cast<const void*>(s.data()), len);
}

// Reads a length-prefixed string. Sets ok=false and returns "" on any read failure;
// subsequent calls with ok=false are no-ops, preserving the error state for the caller.
std::string readString(HalFile& f, bool& ok) {
  if (!ok) return {};
  uint16_t len = 0;
  if (f.read(&len, sizeof(len)) != sizeof(len)) {
    ok = false;
    return {};
  }
  if (len == 0) return {};
  std::string s(len, '\0');
  if (f.read(s.data(), len) != static_cast<int>(len)) {
    ok = false;
    return {};
  }
  return s;
}

// Binary cache entry format (/.tmp_opds_cache.dat):
//   uint8_t  type (OpdsEntryType)
//   u16+str  title | author | href | id | summary | imageHref
//   uint16_t numLinks
//   per link: u16+str href | mimeType | formatKey | fileExtension
// All multi-byte integers are native-endian. The file is session-scoped and
// never persisted across reboots, so no versioning is needed.
void writeEntryToCache(HalFile& f, const OpdsEntry& entry) {
  uint8_t type = static_cast<uint8_t>(entry.type);
  f.write(&type, sizeof(type));
  writeString(f, entry.title);
  writeString(f, entry.author);
  writeString(f, entry.href);
  writeString(f, entry.id);
  writeString(f, entry.summary);
  writeString(f, entry.imageHref);
  uint16_t numLinks = entry.acquisitionLinks.size();
  f.write(reinterpret_cast<const uint8_t*>(&numLinks), sizeof(numLinks));
  for (const auto& link : entry.acquisitionLinks) {
    writeString(f, link.href);
    writeString(f, link.mimeType);
    writeString(f, link.formatKey);
    writeString(f, link.fileExtension);
  }
}

OpdsEntry readEntryFromCache(HalFile& f) {
  OpdsEntry entry;
  uint8_t type = 0;
  if (f.read(&type, sizeof(type)) != sizeof(type)) return {};
  entry.type = static_cast<OpdsEntryType>(type);
  bool ok = true;
  entry.title = readString(f, ok);
  entry.author = readString(f, ok);
  entry.href = readString(f, ok);
  entry.id = readString(f, ok);
  entry.summary = readString(f, ok);
  entry.imageHref = readString(f, ok);
  if (!ok) return {};
  uint16_t numLinks = 0;
  if (f.read(&numLinks, sizeof(numLinks)) != sizeof(numLinks)) return {};
  for (uint16_t i = 0; i < numLinks; ++i) {
    OpdsAcquisitionLink link;
    link.href = readString(f, ok);
    link.mimeType = readString(f, ok);
    link.formatKey = readString(f, ok);
    link.fileExtension = readString(f, ok);
    if (!ok) return {};
    entry.acquisitionLinks.push_back(std::move(link));
  }
  return entry;
}

// Per-server discovery cache (/.crosspoint/opds_XXXXXXXX_disc.json):
// Stores the root feed's nav entries and search template so the browser can
// open instantly without a network round-trip. The filename is derived from a
// 32-bit FNV-1a hash of the server URL. Format:
//   { "v": 1, "st": "<search-template>", "nav": [{"t":"<title>","h":"<href>"}] }
// Increment "v" if the schema changes so old caches are automatically discarded.

std::string opdsDiscoveryPath(const std::string& serverUrl) {
  uint32_t h = 2166136261u;
  for (unsigned char c : serverUrl) {
    h ^= c;
    h *= 16777619u;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "/.crosspoint/opds_%08lx_disc.json", static_cast<unsigned long>(h));
  return buf;
}

// Reads the discovery cache for serverUrl. On success, writes the cached nav
// entries into an already-open cacheFile and appends their offsets to entryOffsets.
// Sets searchTemplateOut. Returns false if the cache is absent or unusable.
bool loadOpdsDiscovery(const std::string& serverUrl, std::string& searchTemplateOut, HalFile& cacheFile,
                       std::vector<uint32_t>& entryOffsets) {
  const std::string path = opdsDiscoveryPath(serverUrl);
  if (!Storage.exists(path.c_str())) return false;
  const String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;

  if (doc["v"].as<int>() != 1) return false;  // unknown or pre-versioned format — discard

  searchTemplateOut = doc["st"] | std::string{};

  const JsonArray arr = doc["nav"].as<JsonArray>();
  if (arr.isNull()) return false;

  for (JsonObject obj : arr) {
    const char* t = obj["t"] | "";
    const char* h = obj["h"] | "";
    if (!t[0] || !h[0]) continue;
    OpdsEntry entry;
    entry.type = OpdsEntryType::NAVIGATION;
    entry.title = t;
    entry.href = h;
    const uint32_t offset = cacheFile.position();
    entryOffsets.push_back(offset);
    writeEntryToCache(cacheFile, entry);
  }

  // A search-only server may have no nav entries but a valid search template — still a usable cache.
  return !entryOffsets.empty() || !searchTemplateOut.empty();
}

void saveOpdsDiscovery(const std::string& serverUrl, const std::string& searchTemplate,
                       const std::vector<std::pair<std::string, std::string>>& navEntries) {
  if (navEntries.empty() && searchTemplate.empty()) return;
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["v"] = 1;
  doc["st"] = searchTemplate;
  JsonArray arr = doc["nav"].to<JsonArray>();
  for (const auto& [title, href] : navEntries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["t"] = title;
    obj["h"] = href;
  }

  String json;
  serializeJson(doc, json);
  const std::string path = opdsDiscoveryPath(serverUrl);
  if (!Storage.writeFile(path.c_str(), json)) {
    LOG_ERR("OPDS", "Failed to save discovery cache: %s", path.c_str());
  } else {
    LOG_DBG("OPDS", "Saved discovery cache: %zu nav entries, search=%s", navEntries.size(),
            searchTemplate.empty() ? "none" : "yes");
  }
}
}  // namespace

OpdsEntry OpdsBookBrowserActivity::getEntry(size_t index) const {
  if (index >= entryOffsets.size()) return {};
  HalFile f;
  if (!Storage.openFileForRead("OPDS", "/.tmp_opds_cache.dat", f)) return {};
  f.seek(entryOffsets[index]);
  OpdsEntry entry = readEntryFromCache(f);
  return entry;
}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entryOffsets.clear();
  navigationHistory.clear();
  currentPath = "";  // Root path - user provides full URL in settings
  searchTemplate.clear();
  selectorIndex = 0;
  selectedBookIndex = -1;
  formatSelectorIndex = 0;
  formatSelectionLabels.clear();
  memoryTrimmed = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  // Try the per-server discovery cache so the browser opens instantly without
  // a network round-trip. Nav entries and search template from the last
  // successful root-feed fetch are written into the tmp cache file so the
  // existing BROWSING render path works unchanged.
  {
    HalFile cacheFile;
    if (Storage.openFileForWrite("OPDS", "/.tmp_opds_cache.dat", cacheFile)) {
      if (loadOpdsDiscovery(server.url, searchTemplate, cacheFile, entryOffsets)) {
        LOG_DBG("OPDS", "Discovery cache hit: %zu nav entries", entryOffsets.size());
        cacheFile.flush();
        cacheFile.close();
        state = BrowserState::BROWSING;
        requestUpdate();
        if (!initialQuery_.empty() && !searchTemplate.empty()) {
          performSearch(std::move(initialQuery_));
          initialQuery_.clear();
        }
        return;  // WiFi connects lazily on first navigation
      }
      cacheFile.close();
      Storage.remove("/.tmp_opds_cache.dat");
    }
  }

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();

  entryOffsets.clear();
  navigationHistory.clear();
  formatSelectionLabels.clear();
  Storage.remove("/.tmp_opds_cache.dat");

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (leavingToFileTransfer) {
      silentRestartToFileTransferOpds();
    } else {
      silentRestart();
    }
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (state == BrowserState::BROWSING) {
    catalogList.update();
    return;
  }

  if (state == BrowserState::FORMAT_SELECTION) {
    if (selectedBookIndex < 0 || selectedBookIndex >= static_cast<int>(entryOffsets.size()) ||
        getEntry(selectedBookIndex).acquisitionLinks.empty()) {
      closeFormatPicker();
      return;
    }
    formatList.update();
    return;
  }

  // The states below are not lists, but they read button events too, never levels: every press is
  // also queued as an event, and a list reads that queue once it is back on screen, so a press read
  // here by level would be handled a second time there.
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    const auto button = event.button;
    if (state == BrowserState::ERROR) {
      if (button == MappedInputManager::Button::Confirm) {
        checkAndConnectWifi();
        return;
      }
      if (button == MappedInputManager::Button::Back) {
        navigateBack();
        return;
      }
    } else if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
      if (button == MappedInputManager::Button::Back) {
        state == BrowserState::CHECK_WIFI ? leave() : navigateBack();
        return;
      }
    } else if (state == BrowserState::BOOK_DETAIL) {
      if (button == MappedInputManager::Button::Back ||
          MappedInputManager::isDirection(button, MappedInputManager::Direction::Right)) {
        state = BrowserState::BROWSING;
        requestUpdate();
        return;
      }
      if (button == MappedInputManager::Button::Confirm) {
        const auto entry = getEntry(selectorIndex);
        state = BrowserState::BROWSING;
        requestUpdate();
        chooseBookFormat(entry);
        return;
      }
    }
    // DOWNLOADING never has presses to read here: the download runs inside loop(), reads Back by level
    // to abort, and drains the queue when it returns.
  }
}

// A catalog list is something you READ, so it has to be allowed to sleep like any
// other list. Only the states with work in flight hold the device awake.
// WIFI_SELECTION and SEARCH_INPUT are the two states where a child activity owns
// the screen and therefore owns its own override; they stay true here to cover the
// gap either side of the handover, when this activity is still the current one.
bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::BOOK_DETAIL:
    case BrowserState::FORMAT_SELECTION:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

int OpdsBookBrowserActivity::catalogRowsPerPage() const {
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const int rows = (contentRect.y + contentRect.height - CATALOG_LIST_TOP) / CATALOG_ROW_HEIGHT;
  return std::max(1, std::min(rows, ListTouchBand::kMaxRows));
}

void OpdsBookBrowserActivity::showBookDetail(const OpdsEntry& entry) {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();
  fetchCoverForEntry(entry);
  state = BrowserState::BOOK_DETAIL;
  requestUpdate();
}

void OpdsBookBrowserActivity::closeFormatPicker() {
  state = BrowserState::BROWSING;
  selectedBookIndex = -1;
  formatSelectionLabels.clear();
  requestUpdate();
}

int OpdsBookBrowserActivity::CatalogHost::listCount() const { return static_cast<int>(browser.entryOffsets.size()); }

ListWindow OpdsBookBrowserActivity::CatalogHost::listWindow() const {
  ListWindow window;
  window.top = browser.catalogTop.load();
  window.drawn = browser.catalogPageRows.load();
  return window;
}

int OpdsBookBrowserActivity::CatalogHost::listSelected() const { return browser.selectorIndex; }

void OpdsBookBrowserActivity::CatalogHost::listSelect(const int row) {
  browser.selectorIndex = row;
  browser.requestUpdate();
}

bool OpdsBookBrowserActivity::CatalogHost::listActionAvailable(const ListGrammar::Side side, const int row) const {
  if (side == ListGrammar::Side::Left) return !browser.searchTemplate.empty();
  // Bounds first: an empty feed has no entry to read.
  return row >= 0 && row < listCount() && browser.getEntry(row).type == OpdsEntryType::BOOK;
}

void OpdsBookBrowserActivity::CatalogHost::onListActivate(const int row, bool /*longPress*/) {
  if (row < 0 || row >= listCount()) return;
  const auto entry = browser.getEntry(row);
  entry.type == OpdsEntryType::BOOK ? browser.chooseBookFormat(entry) : browser.navigateToEntry(entry);
}

void OpdsBookBrowserActivity::CatalogHost::onListBack() { browser.navigateBack(); }

void OpdsBookBrowserActivity::CatalogHost::onListHome() { browser.onGoHome(); }

void OpdsBookBrowserActivity::CatalogHost::onListAction(const ListGrammar::Side side, const int row) {
  if (side == ListGrammar::Side::Left) {
    browser.launchSearch();
    return;
  }
  browser.showBookDetail(browser.getEntry(row));
}

int OpdsBookBrowserActivity::FormatHost::listCount() const {
  return static_cast<int>(browser.formatSelectionLabels.size());
}

ListWindow OpdsBookBrowserActivity::FormatHost::listWindow() const {
  ListWindow window;
  window.top = browser.formatTop.load();
  window.drawn = browser.formatPageRows.load();
  return window;
}

int OpdsBookBrowserActivity::FormatHost::listSelected() const { return browser.formatSelectorIndex; }

void OpdsBookBrowserActivity::FormatHost::listSelect(const int row) {
  browser.formatSelectorIndex = row;
  browser.requestUpdate();
}

void OpdsBookBrowserActivity::FormatHost::onListActivate(const int row, bool /*longPress*/) {
  const auto entry = browser.getEntry(browser.selectedBookIndex);
  if (row < 0 || row >= static_cast<int>(entry.acquisitionLinks.size())) return;
  browser.downloadBook(entry, entry.acquisitionLinks[row]);
}

void OpdsBookBrowserActivity::FormatHost::onListBack() { browser.closeFormatPicker(); }

void OpdsBookBrowserActivity::FormatHost::onListHome() { browser.onGoHome(); }

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // The two lists, the catalog and the format picker, label their side buttons; every other state
  // uses Back and Confirm alone, so it keeps the full width.
  const bool showsList = state == BrowserState::BROWSING || state == BrowserState::FORMAT_SELECTION;
  const Rect contentRect = UITheme::getContentRect(renderer, true, showsList);
  const int midY = contentRect.y + contentRect.height / 2;

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 40, tr(STR_DOWNLOADING));
    // Trim long titles to keep them within the content bounds.
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), contentRect.width - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 10, title.c_str());
    if (downloadTotal > 0) {
      const int barWidth = contentRect.width - 100;
      constexpr int barHeight = 20;
      const int barX = contentRect.x + 50;
      const int barY = midY + 20;
      GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadProgress, downloadTotal);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::FORMAT_SELECTION) {
    const auto entry = getEntry(selectedBookIndex);
    auto title = renderer.truncatedText(UI_10_FONT_ID, entry.title.c_str(), contentRect.width - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 40, title.c_str(), true, EpdFontFamily::BOLD);
    if (!entry.author.empty()) {
      auto author = renderer.truncatedText(UI_10_FONT_ID, entry.author.c_str(), contentRect.width - 40);
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 10, author.c_str());
    }

    const int listTop = midY + 20;
    const int itemsPerPage = formatItemsPerPage(contentRect);
    formatPageRows.store(itemsPerPage);
    const int pageStartIndex = formatSelectorIndex / itemsPerPage * itemsPerPage;
    formatTop.store(pageStartIndex);
    // Format rows published for touch. This screen has TWO lists in two different states, and
    // only one is on screen at a time; the recorders are cleared at the top of each render pass,
    // so whichever state painted last is the only one a tap can reach.
    ListTouchBand::recordUniformRows(
        contentRect.x, contentRect.width - 1, listTop - 2, FORMAT_ITEM_HEIGHT, pageStartIndex,
        std::min(itemsPerPage, static_cast<int>(entry.acquisitionLinks.size()) - pageStartIndex));
    renderer.fillRect(contentRect.x, listTop + (formatSelectorIndex - pageStartIndex) * FORMAT_ITEM_HEIGHT - 2,
                      contentRect.width - 1, FORMAT_ITEM_HEIGHT);
    for (int i = pageStartIndex;
         i < static_cast<int>(entry.acquisitionLinks.size()) && i < pageStartIndex + itemsPerPage; i++) {
      const char* label = i < static_cast<int>(formatSelectionLabels.size()) ? formatSelectionLabels[i].c_str() : "";
      auto item = renderer.truncatedText(UI_10_FONT_ID, label, contentRect.width - 40);
      renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, listTop + (i - pageStartIndex) * FORMAT_ITEM_HEIGHT,
                        item.c_str(), i != formatSelectorIndex);
    }

    formatList.drawHints(renderer, tr(STR_BACK), tr(STR_DOWNLOAD));
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::BOOK_DETAIL) {
    const auto entry = getEntry(selectorIndex);
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int fmtY = contentRect.y + contentRect.height - lineH - 4;

    // Cover: draw on the left if available
    int coverColW = 0;
    if (coverAvailable) {
      HalFile bmpFile;
      if (Storage.openFileForRead("OPDS", "/.tmp_opds_cover.bmp", bmpFile)) {
        Bitmap bmp(bmpFile);
        if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
          const int coverAreaH = fmtY - 35 - 4;
          int coverH = std::min(bmp.getHeight(), coverAreaH);
          int coverW = bmp.getWidth() * coverH / bmp.getHeight();
          if (coverW > contentRect.width / 3) {
            coverW = contentRect.width / 3;
            coverH = bmp.getHeight() * coverW / bmp.getWidth();
          }
          renderer.drawBitmap(bmp, contentRect.x, 35, coverW, coverH);
          coverColW = coverW + 6;
        }
        bmpFile.close();
      }
    }

    // Text column (right of cover, or full width)
    const int textX = contentRect.x + coverColW + 10;
    const int textW = contentRect.width - coverColW - 20;
    int y = 35;

    auto title = renderer.truncatedText(UI_10_FONT_ID, entry.title.c_str(), textW, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, textX, y, title.c_str(), true, EpdFontFamily::BOLD);
    y += lineH + 2;

    if (!entry.author.empty()) {
      auto author = renderer.truncatedText(UI_10_FONT_ID, entry.author.c_str(), textW);
      renderer.drawText(UI_10_FONT_ID, textX, y, author.c_str());
      y += lineH + 2;
    }

    renderer.drawLine(contentRect.x + coverColW, y, contentRect.x + contentRect.width - 1, y);
    y += 5;

    if (!entry.summary.empty()) {
      const int maxLines = (fmtY - 8 - y) / lineH;
      if (maxLines > 0) {
        const auto lines = renderer.wrappedText(UI_10_FONT_ID, entry.summary.c_str(), textW, maxLines);
        for (const auto& line : lines) {
          renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str());
          y += lineH;
        }
      }
    }

    if (!entry.acquisitionLinks.empty()) {
      std::string fmts;
      for (const auto& link : entry.acquisitionLinks) {
        if (!fmts.empty()) fmts += " · ";
        fmts += link.formatKey;
      }
      auto fmtsStr = renderer.truncatedText(UI_10_FONT_ID, fmts.c_str(), contentRect.width - 20);
      renderer.drawCenteredText(UI_10_FONT_ID, fmtY, fmtsStr.c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Browsing state. The selected entry's type decides the Confirm label; the Left/Right labels
  // come from the catalog's declaration (Search / Info, or the page glyph alone).
  const bool selectedIsBook = !entryOffsets.empty() && getEntry(selectorIndex).type == OpdsEntryType::BOOK;
  catalogList.drawHints(renderer, tr(STR_BACK), selectedIsBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN));

  if (entryOffsets.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_NO_ENTRIES));
    renderer.displayBuffer();
    return;
  }

  const int rowsPerPage = catalogRowsPerPage();
  catalogPageRows.store(rowsPerPage);
  const auto pageStartIndex = selectorIndex / rowsPerPage * rowsPerPage;
  catalogTop.store(pageStartIndex);
  // Entry rows published for touch. The top is a bare CATALOG_LIST_TOP, NOT contentRect.y + it as
  // the chapter selectors use — matched to the fill below, which is where the row visibly is.
  ListTouchBand::recordUniformRows(contentRect.x, contentRect.width - 1, CATALOG_LIST_TOP - 2, CATALOG_ROW_HEIGHT,
                                   pageStartIndex,
                                   std::min<int>(rowsPerPage, static_cast<int>(entryOffsets.size()) - pageStartIndex));
  renderer.fillRect(contentRect.x, CATALOG_LIST_TOP + (selectorIndex % rowsPerPage) * CATALOG_ROW_HEIGHT - 2,
                    contentRect.width - 1, CATALOG_ROW_HEIGHT);

  for (size_t i = pageStartIndex; i < entryOffsets.size() && i < static_cast<size_t>(pageStartIndex + rowsPerPage);
       i++) {
    const auto entry = getEntry(i);

    // Format display text with type indicator
    std::string displayText;
    if (entry.type == OpdsEntryType::NAVIGATION) {
      displayText = "> " + entry.title;  // Folder/navigation indicator
    } else {
      // Book: "Title - Author" or just "Title"
      displayText = entry.title;
      if (!entry.author.empty()) {
        displayText += " - " + entry.author;
      }
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), contentRect.width - 40);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, CATALOG_LIST_TOP + (i % rowsPerPage) * CATALOG_ROW_HEIGHT,
                      item.c_str(), i != static_cast<size_t>(selectorIndex));
  }

  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  // Trim memory once per session before the first TLS connection, regardless of
  // whether we arrived here via the WiFi-first path or directly from a cache hit.
  if (!memoryTrimmed) {
    trimMemoryForNetworkSession(renderer, "OPDS");
    memoryTrimmed = true;
  }

  entryOffsets.clear();
  // Reserve up front so the per-entry push_back loop doesn't repeatedly realloc
  // (2x grow + copy) and fragment the heap mid-fetch. One page-ish worth covers
  // the common case; larger feeds grow once or twice up to MAX_FEED_ENTRIES.
  entryOffsets.reserve(FEED_RESERVE_ENTRIES);
  bool feedCapped = false;

  // Root feed: path is empty (first open or back-to-root navigation).
  const bool isRootFeed = path.empty();
  std::vector<std::pair<std::string, std::string>> capturedNavEntries;

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());

  OpdsParser parser;
  HalFile cacheFile;

  if (!Storage.openFileForWrite("OPDS", "/.tmp_opds_cache.dat", cacheFile)) {
    LOG_ERR("OPDS", "Could not open cache file");
    state = BrowserState::ERROR;
    errorMessage = "Cache Error";
    requestUpdate();
    return;
  }

  parser.onEntryParsed = [&](const OpdsEntry& entry) {
    if (entryOffsets.size() >= MAX_FEED_ENTRIES) {
      if (!feedCapped) {
        feedCapped = true;
        LOG_ERR("OPDS", "Feed exceeds %zu entries; ignoring the rest. Use a paginated catalog.", MAX_FEED_ENTRIES);
      }
      return;  // drop the excess rather than grow the offset index unbounded
    }
    uint32_t offset = cacheFile.position();
    entryOffsets.push_back(offset);
    writeEntryToCache(cacheFile, entry);
    if (isRootFeed && entry.type == OpdsEntryType::NAVIGATION) {
      capturedNavEntries.push_back({entry.title, entry.href});
    }
  };

  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  // yxml has fixed-capacity buffers; a non-zero mask means a field (commonly a
  // long acquisition href with query params, or a long title) was silently
  // truncated. Log it so field cases that exceed the bounds are diagnosable.
  if (const uint32_t trunc = parser.truncationFlags()) {
    LOG_ERR("OPDS", "Feed parse truncated some fields (flags=0x%lx)", static_cast<unsigned long>(trunc));
  }

  searchTemplate = parser.getSearchTemplate();
  if (searchTemplate.empty() && !parser.getOsdUrl().empty()) {
    fetchOsdTemplate(UrlUtils::buildUrl(url, parser.getOsdUrl()));
  } else if (!initialQuery_.empty() && !searchTemplate.empty()) {
    performSearch(std::move(initialQuery_));
    initialQuery_.clear();
  }

  if (isRootFeed) {
    saveOpdsDiscovery(server.url, searchTemplate, capturedNavEntries);
  }

  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();

  if (!prevUrl.empty()) {
    std::string resolvedPrevUrl = UrlUtils::buildUrl(url, prevUrl);
    OpdsEntry prevEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", resolvedPrevUrl, ""};
    entryOffsets.insert(entryOffsets.begin(), cacheFile.position());
    writeEntryToCache(cacheFile, prevEntry);
  }
  if (!nextUrl.empty()) {
    std::string resolvedNextUrl = UrlUtils::buildUrl(url, nextUrl);
    OpdsEntry nextEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", resolvedNextUrl, ""};
    entryOffsets.push_back(cacheFile.position());
    writeEntryToCache(cacheFile, nextEntry);
  }

  selectorIndex = 0;
  state = entryOffsets.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entryOffsets.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  currentPath = entry.href;
  entryOffsets.clear();
  selectorIndex = 0;
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    leave();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    entryOffsets.clear();
    selectorIndex = 0;
    checkAndConnectWifi();
  }
}

void OpdsBookBrowserActivity::leave() {
  switch (backTo) {
    case OpdsBackTo::FileTransfer:
      leavingToFileTransfer = true;
      activityManager.goToFileTransfer(NetworkMode::OPDS_BROWSER);
      return;
    case OpdsBackTo::Home:
      onGoHome();
      return;
  }
}

// Opens a screen to allow the user to choose which format they want to download
// if multiple formats are available. If only one format is available, the
// download is started immediately.
void OpdsBookBrowserActivity::chooseBookFormat(const OpdsEntry& book) {
  if (book.acquisitionLinks.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  if (book.acquisitionLinks.size() == 1) {
    downloadBook(book, book.acquisitionLinks[0]);
    return;
  }

  selectedBookIndex = selectorIndex;
  formatSelectorIndex = 0;
  formatSelectionLabels = buildOpdsFormatSelectionLabels(book.acquisitionLinks, server.url);
  state = BrowserState::FORMAT_SELECTION;
  requestUpdate();
}

// Downloads the selected book's acquisition link and saves it to the SD card.
void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book, const OpdsAcquisitionLink& acquisition) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = 0;
  downloadTotal = 0;
  requestUpdate(true);

  std::string downloadUrl =
      (acquisition.href.rfind("http", 0) == 0) ? acquisition.href : UrlUtils::buildUrl(server.url, acquisition.href);
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
    // existing caller checks exists() before mkdir. On real failure, fall back
    // to SD root so the download is never lost.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // downloadToFile() needs a std::string, and titles are unbounded (a fixed
  // char[] would truncate). Cold path (a multi-second download follows), so one
  // reserve'd, in-place-appended owning string is the right call.
  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat),
                               acquisition.fileExtension);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  uint32_t lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastProgressUpdateMs](const unsigned int downloaded, const unsigned int total) {
        mappedInput.update();
        downloadProgress = downloaded;
        downloadTotal = total;
        const uint32_t now = millis();
        if (now - lastProgressUpdateMs >= 3000) {
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
        return !mappedInput.wasPressed(MappedInputManager::Button::Back);
      },
      server.username, server.password);
  // The download read Back by level to abort. Drop the events it left behind, or that same press
  // would reach the catalog afterwards as an event and leave the feed.
  buttonEvents.drain();

  if (result == HttpDownloader::OK) {
    FsFile downloadedFile;
    if (!Storage.openFileForRead("OPDS", filename, downloadedFile) || downloadedFile.size() == 0) {
      LOG_ERR("OPDS", "Downloaded file is empty or unreadable: %s", filename.c_str());
      if (downloadedFile) {
        downloadedFile.close();
      }
      Storage.remove(filename.c_str());
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    downloadedFile.close();

    LOG_DBG("OPDS", "Download complete: %s", filename.c_str());

    // Clear any existing cache for this book just in case it's a redownload of
    // a previously opened book.
    if (acquisition.mimeType == "application/epub+zip") {
      if (!book.imageHref.empty()) {
        const std::string coverUrl =
            (book.imageHref.rfind("http", 0) == 0) ? book.imageHref : UrlUtils::buildUrl(server.url, book.imageHref);

        std::string baseFilename = filename;
        size_t dotPos = baseFilename.find_last_of('.');
        if (dotPos != std::string::npos) {
          baseFilename.resize(dotPos);
        }

        std::string ext = ".jpg";
        if (book.imageHref.length() >= 4) {
          std::string lowerHref = book.imageHref.substr(book.imageHref.length() - 4);
          std::transform(lowerHref.begin(), lowerHref.end(), lowerHref.begin(), ::tolower);
          if (lowerHref == ".png") {
            ext = ".png";
          }
        }
        std::string sidecarPath = baseFilename + ext;

        // Reset progress so the bar tracks the cover, not the just-finished book
        // (otherwise it shows the book's total frozen at 100%).
        downloadProgress = 0;
        downloadTotal = 0;
        requestUpdate(true);

        uint32_t lastCoverProgressMs = 0;
        const auto coverDlResult = HttpDownloader::downloadToFile(
            coverUrl, sidecarPath,
            [this, &lastCoverProgressMs](const unsigned int downloaded, const unsigned int total) {
              mappedInput.update();
              downloadProgress = downloaded;
              downloadTotal = total;
              const uint32_t now = millis();
              if (now - lastCoverProgressMs >= 3000) {  // throttle redraws like the book download
                lastCoverProgressMs = now;
                requestUpdate(true);
              }
              return !mappedInput.wasPressed(MappedInputManager::Button::Back);
            },
            server.username, server.password);
        buttonEvents.drain();
        if (coverDlResult != HttpDownloader::OK) {
          LOG_ERR("OPDS", "Failed to download cover from %s (err %d)", coverUrl.c_str(), (int)coverDlResult);
          Storage.remove(sidecarPath.c_str());
        }
      }

      Epub epub(filename, "/.crosspoint");
      epub.clearCache();
    } else if (acquisition.formatKey == "xtc" || acquisition.formatKey == "xtch") {
      Xtc(filename, "/.crosspoint").clearCache();
    }
    selectedBookIndex = -1;
    formatSelectionLabels.clear();
    state = BrowserState::BROWSING;
    requestUpdate();
  } else if (result == HttpDownloader::ABORTED) {
    selectedBookIndex = -1;
    formatSelectionLabels.clear();
    state = BrowserState::BROWSING;
    requestUpdate();
  } else {
    selectedBookIndex = -1;
    formatSelectionLabels.clear();
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
}

void OpdsBookBrowserActivity::fetchOsdTemplate(const std::string& osdUrl) {
  std::string content;
  if (!HttpDownloader::fetchUrl(osdUrl, content)) {
    LOG_ERR("OPDS", "Failed to fetch OSD: %s", osdUrl.c_str());
    return;
  }
  // OSD documents are typically a few hundred bytes. Reject suspiciously large
  // responses before handing them to the XML parser to avoid heap exhaustion.
  constexpr size_t kOsdMaxBytes = 64 * 1024;
  if (content.size() > kOsdMaxBytes) {
    LOG_ERR("OPDS", "OSD response too large (%zu bytes), ignoring", content.size());
    return;
  }

  struct OsdState {
    std::string templateUrl;
    static void onStart(void* ud, const char* name, const char** atts) {
      if (strcmp(name, "Url") != 0 && strstr(name, ":Url") == nullptr) return;
      auto* state = static_cast<OsdState*>(ud);
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "template") == 0 && strstr(atts[i + 1], "{searchTerms}") != nullptr) {
          state->templateUrl = atts[i + 1];
          break;
        }
      }
    }
  } osdState;

  SaxParser parser;
  if (!parser.init(&osdState, OsdState::onStart, nullptr)) {
    LOG_ERR("OPDS", "OSD parser alloc failed");
    return;
  }
  if (!parser.feed(reinterpret_cast<const uint8_t*>(content.data()), content.size()) || !parser.finalize()) {
    LOG_ERR("OPDS", "OSD parse error at line %d: %s", parser.errorLine(), parser.errorString());
  }

  if (!osdState.templateUrl.empty()) {
    searchTemplate = UrlUtils::buildUrl(osdUrl, osdState.templateUrl);
    if (!initialQuery_.empty()) {
      performSearch(std::move(initialQuery_));
      initialQuery_.clear();
    }
  }
}

void OpdsBookBrowserActivity::launchSearch() {
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::fetchCoverForEntry(const OpdsEntry& entry) {
  coverAvailable = false;
  Storage.remove("/.tmp_opds_cover.bmp");
  if (entry.imageHref.empty()) return;

  const std::string coverUrl =
      (entry.imageHref.rfind("http", 0) == 0) ? entry.imageHref : UrlUtils::buildUrl(server.url, entry.imageHref);
  LOG_DBG("OPDS", "Fetching cover: %s", coverUrl.c_str());

  if (HttpDownloader::downloadToFile(coverUrl, "/.tmp_opds_cover.jpg", nullptr, server.username, server.password) !=
      HttpDownloader::OK) {
    LOG_DBG("OPDS", "Cover download failed");
    return;
  }

  const int coverMaxW = renderer.getScreenWidth() / 3;
  const int coverMaxH = coverMaxW * 4 / 3;

  HalFile src;
  if (!Storage.openFileForRead("OPDS", "/.tmp_opds_cover.jpg", src)) return;
  HalFile dst;
  if (!Storage.openFileForWrite("OPDS", "/.tmp_opds_cover.bmp", dst)) {
    src.close();
    return;
  }

  const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(src, dst, coverMaxW, coverMaxH);
  src.close();
  dst.close();
  Storage.remove("/.tmp_opds_cover.jpg");

  if (ok) {
    coverAvailable = true;
    LOG_DBG("OPDS", "Cover ready");
  } else {
    Storage.remove("/.tmp_opds_cover.bmp");
    LOG_INF("OPDS", "Cover JPEG conversion failed (progressive JPEG unsupported; see JPG log for details)");
  }
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);
  currentPath = url;
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  // Already connected? Verify connection is valid by checking IP
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without further fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

ListRowTap::Result OpdsBookBrowserActivity::selectListRow(const int index) {
  // Two lists, two states. `state` is read on the loop task and may have moved on since the render
  // that recorded the band, so it decides which list a tap means.
  if (state == BrowserState::FORMAT_SELECTION) return formatList.tapRow(index);
  if (state == BrowserState::BROWSING) return catalogList.tapRow(index);
  return ListRowTap::Result::Rejected;
}

bool OpdsBookBrowserActivity::pageList(const ListPageDirection direction) {
  const int step = direction == ListPageDirection::Forward ? 1 : -1;
  if (state == BrowserState::BROWSING) {
    catalogList.page(step);
  } else if (state == BrowserState::FORMAT_SELECTION) {
    formatList.page(step);
  }
  return true;
}
