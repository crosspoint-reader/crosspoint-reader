#pragma once

#include <Epub/Page.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

// Controller for dictionary lookup: renders the background page, runs the
// lookup against the active StarDict dictionary, and either opens
// DictionaryDefinitionActivity or displays a status popup (Looking up,
// Indexing, Not found, Error).
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string lookupText = {})
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        lookupText(std::move(lookupText)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  size_t dictionaryCount() const { return dictionaries.size(); }
  const char* dictionaryName(size_t index) const { return dictionaries[index].name.c_str(); }
  bool hasSearchCache(size_t index, const char* word) const;
  bool lookupInDictionary(size_t index, const char* word, std::string& definition, std::string& headword,
                          Dictionary::LookupResult* result);
  void prefetch(const std::string& query);
  void stopPrefetch();

 private:
  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void performLookup();
  void runPrefetch();
  static void prefetchTask(void* context);
  static void searchCachePath(size_t index, char* path, size_t capacity);

  std::vector<DictionaryEntry> dictionaries;
  std::string prefetchQuery;
  std::atomic<size_t> prefetched{0};
  std::atomic<bool> prefetchCancelled{false};
  std::atomic<bool> prefetchStopped{true};
  TaskHandle_t worker = nullptr;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;

  std::string lookupText;
  bool lookupPending = false;
  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
};
