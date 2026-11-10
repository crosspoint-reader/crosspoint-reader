#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "ReaderTextSettings.h"
#include "activities/Activity.h"
#include "util/Dictionary.h"
#include "vocabulary/VocabularyStore.h"

// Controller for dictionary lookup: renders the background page, runs the
// lookup against the active StarDict dictionary, and either opens
// DictionaryDefinitionActivity or displays a status popup (Looking up,
// Indexing, Not found, Error).
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string lookupText = {}, VocabularyContext vocabulary = {},
                                        const ReaderTextSettings* textSettings = nullptr, int readerFontId = 0);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void performLookup();

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  const ReaderTextSettings textSettings;
  const int fontId;

  std::string lookupText;
  VocabularyContext vocabulary;
  bool lookupPending = false;
  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
};
