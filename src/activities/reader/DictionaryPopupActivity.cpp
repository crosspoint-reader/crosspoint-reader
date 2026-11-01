#include "DictionaryPopupActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "util/HtmlToPlainText.h"

#include <algorithm>

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;
// Definitions longer than this are truncated with an ellipsis line.
constexpr size_t MAX_DEFINITION_BYTES = 12 * 1024;

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryPopupActivity::onEnter() {
  TextPopupActivity::onEnter();
  lookupPending = true;
  popup = Popup::Busy;
  popupMsg = StrId::STR_DICT_LOOKING_UP;
  requestUpdate();
}

void DictionaryPopupActivity::performLookup() {
  {
    RenderLock lock;
    popupMsg = StrId::STR_DICT_LOOKING_UP;
    popup = Popup::Busy;
  }
  // Mirror the full-screen flow's heap hygiene before touching the SD/heap:
  // release rebuildable font caches so the definition extraction keeps its
  // contiguous-block budget.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("DPOP", "lookup '%s': free=%u largest=%u", lookupText.c_str(), (unsigned)heap.freeBytes,
            (unsigned)heap.largestBlockBytes);
  }
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  {
    RenderLock lock;
    popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  }
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;
  }

  std::string definition;
  std::string foundHeadword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = ok && dict.lookup(lookupText.c_str(), definition, foundHeadword, &result);
  {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("DPOP", "lookup result=%d found=%d defSize=%u free=%u largest=%u", (int)result, (int)found,
            (unsigned)definition.size(), (unsigned)heap.freeBytes, (unsigned)heap.largestBlockBytes);
  }

  if (found) {
    const bool isHtml = dict.definitionsAreHtml();
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
    if (isHtml) definition = htmlToPlainText(definition, DICTIONARY_PLAIN_HEADWORD_MARKER);
    if (definition.size() > MAX_DEFINITION_BYTES) {
      definition.resize(MAX_DEFINITION_BYTES);
      definition += "\n…";
    }
    definition.erase(std::remove(definition.begin(), definition.end(), DICTIONARY_PLAIN_HEADWORD_MARKER),
                     definition.end());
    {
      RenderLock lock;
      popup = Popup::None;
      // Keep `page`: unlike the full-screen flow (which frees it before
      // laying out a definition), the popup renders the page as its background.
      const bool shown = showText(std::move(foundHeadword), std::move(definition));
      if (!shown) {
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        popupTime = millis();
      } else {
        phase = Phase::Show;
      }
    }
    requestUpdate();
    return;
  }

  // Error naming mirrors DictionaryWordSelectActivity.
  if (!ok) {
    popup = Popup::Error;
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::LookupResult::NotFound:
      default:
        popup = Popup::NotFound;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        break;
    }
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryPopupActivity::loop() {
  if (lookupPending) {
    lookupPending = false;
    performLookup();
    return;
  }
  if (phase != Phase::Show) {
    if ((popup == Popup::NotFound || popup == Popup::Error) && millis() - popupTime >= POPUP_DURATION_MS) {
      finish();
    }
    return;
  }
  TextPopupActivity::loop();
}

void DictionaryPopupActivity::render(RenderLock&& lock) {
  if (phase == Phase::Show) {
    TextPopupActivity::render(std::move(lock));
    return;
  }
  renderer.clearScreen();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);
  if (popup != Popup::None) {
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
