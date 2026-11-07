#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;
constexpr const char* SEARCH_CACHE_DIR = "/.crosspoint/dict-search";
// Index scans stream through a heap buffer; this stack never inflates or lays out definitions.
constexpr size_t PREFETCH_STACK_BYTES = 6144;

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  DictionaryRegistry::discover(dictionaries);
  const auto selected = std::find_if(dictionaries.begin(), dictionaries.end(), [](const DictionaryEntry& entry) {
    return entry.name == SETTINGS.dictionaryName;
  });
  if (selected != dictionaries.end()) std::rotate(dictionaries.begin(), selected, dictionaries.end());
  lookupPending = true;
  popupMsg = StrId::STR_DICT_LOOKING_UP;
  popup = Popup::Busy;
  requestUpdate();
}

void DictionaryWordSelectActivity::searchCachePath(const size_t index, char* path, const size_t capacity) {
  snprintf(path, capacity, "%s/%u", SEARCH_CACHE_DIR, static_cast<unsigned>(index));
}

void DictionaryWordSelectActivity::stopPrefetch() {
  if (!worker) return;
  prefetchCancelled = true;
  // Cooperative shutdown lets HalFiles and scan buffers unwind before task deletion.
  while (!prefetchStopped.load()) vTaskDelay(1);
  vTaskDelete(worker);
  worker = nullptr;
}

void DictionaryWordSelectActivity::onExit() {
  stopPrefetch();
  for (size_t i = 0; i < dictionaries.size(); ++i) {
    char path[64];
    searchCachePath(i, path, sizeof(path));
    if (Storage.exists(path)) Storage.remove(path);
  }
  Activity::onExit();
}

void DictionaryWordSelectActivity::prefetch(const std::string& query) {
  if (dictionaries.size() < 2) return;
  stopPrefetch();
  if (query != prefetchQuery) {
    const auto heap = HalMemory::getDefaultHeap();
    if (heap.freeBytes < query.size() + PREFETCH_STACK_BYTES + 16 * 1024 ||
        heap.largestBlockBytes < std::max(query.size() + 1, PREFETCH_STACK_BYTES)) {
      LOG_ERR("DICT", "Low heap for background search");
      return;
    }
    std::string{}.swap(prefetchQuery);
    prefetchQuery = query;
    prefetched = 0;
  }
  if (prefetched.load() == dictionaries.size()) return;
  if (!Storage.ensureDirectoryExists(SEARCH_CACHE_DIR)) return;
  prefetchCancelled = false;
  prefetchStopped = false;
  if (xTaskCreate(prefetchTask, "dict-search", PREFETCH_STACK_BYTES, this, 1, &worker) != pdPASS) {
    worker = nullptr;
    prefetchStopped = true;
    LOG_ERR("DICT", "Cannot allocate %u byte prefetch stack", static_cast<unsigned>(PREFETCH_STACK_BYTES));
  }
}

void DictionaryWordSelectActivity::prefetchTask(void* context) {
  auto* owner = static_cast<DictionaryWordSelectActivity*>(context);
  owner->runPrefetch();
  owner->prefetchStopped = true;
  vTaskSuspend(nullptr);
}

void DictionaryWordSelectActivity::runPrefetch() {
  HalPowerManager::Lock powerLock;
  auto dictionary = makeUniqueNoThrow<Dictionary>();
  if (!dictionary) {
    LOG_ERR("DICT", "OOM: prefetch dictionary (%u bytes)", static_cast<unsigned>(sizeof(Dictionary)));
    return;
  }
  for (size_t i = prefetched.load(); i < dictionaries.size() && !prefetchCancelled.load(); ++i) {
    char path[64];
    searchCachePath(i, path, sizeof(path));
    if (Storage.exists(path)) Storage.remove(path);
    const bool ok = dictionary->open(dictionaryName(i)) &&
                    dictionary->buildIndex(indexBuildYield, nullptr, nullptr, &prefetchCancelled) &&
                    !prefetchCancelled.load() && dictionary->cacheSearch(prefetchQuery.c_str(), path);
    if (!ok && Storage.exists(path)) Storage.remove(path);
    if (prefetchCancelled.load()) return;
    // Publish only after the snapshot's HalFile has closed.
    prefetched = i + 1;
    vTaskDelay(1);
  }
}

bool DictionaryWordSelectActivity::lookupInDictionary(const size_t index, const char* word, std::string& definition,
                                                      std::string& headword, Dictionary::LookupResult* result) {
  // Release the worker working set before decoding and laying out a definition.
  stopPrefetch();
  *result = Dictionary::LookupResult::ReadError;
  if (!dict.open(dictionaryName(index))) return false;
  if (dict.needsIndex()) {
    auto indexResult = Dictionary::IndexResult::Ok;
    if (!dict.buildIndex(indexBuildYield, nullptr, &indexResult)) {
      if (indexResult == Dictionary::IndexResult::LowMemory) *result = Dictionary::LookupResult::LowMemory;
      return false;
    }
  }
  char path[64];
  searchCachePath(index, path, sizeof(path));
  return dict.lookup(word, definition, headword, result, hasSearchCache(index, word) ? path : nullptr);
}

bool DictionaryWordSelectActivity::hasSearchCache(const size_t index, const char* word) const {
  if (word != prefetchQuery || index >= prefetched.load()) return false;
  char path[64];
  searchCachePath(index, path, sizeof(path));
  return Storage.exists(path);
}

void DictionaryWordSelectActivity::performLookup() {
  {
    RenderLock lock;
    popupMsg = StrId::STR_DICT_LOOKING_UP;
    popup = Popup::Busy;
  }
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    // needsIndex() opens and validates the .qidx sidecar, so ask it once per
    // open rather than once per word: the answer only changes when we build
    // the sidecar ourselves, which is handled below.
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
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = ok && dict.lookup(lookupText.c_str(), definition, headword, &result);

  if (found || (ok && result == Dictionary::LookupResult::NotFound && dictionaries.size() > 1)) {
    if (!found) {
      headword = lookupText;
      definition = tr(STR_DICT_NOT_FOUND);
    }
    {
      RenderLock lock;
      popup = Popup::None;
      page.reset();
    }
    auto activity = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                    std::move(definition), dict.definitionsAreHtml());
    if (!activity || !activity->enableLookup(dict, std::move(lookupText), this, found)) {
      LOG_ERR("DICT", "OOM: definition activity");
      popup = Popup::Error;
      popupMsg = StrId::STR_DICT_LOW_MEMORY;
      popupTime = millis();
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
      setResult(ActivityResult{result});
      finish();
    });
    return;
  }
  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error — and we distinguish decompression from a
  // low-memory allocation from a generic read error.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap — name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
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

void DictionaryWordSelectActivity::loop() {
  if (lookupPending) {
    lookupPending = false;
    performLookup();
    return;
  }
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      result.isCancelled = popup == Popup::Error;
      finish();
    }
  }
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
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
