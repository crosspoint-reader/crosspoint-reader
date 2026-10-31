#pragma once

#include <I18n.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "activities/util/TextPopupActivity.h"
#include "util/Dictionary.h"

// Popup-style dictionary lookup: renders the background page with the word's
// definition framed over it instead of opening the full-screen
// DictionaryDefinitionActivity. Enabled by SETTINGS.dictionaryPopup; Back
// closes the popup and returns straight to reading. The lookup/busy/error
// phase mirrors DictionaryWordSelectActivity; the framed display comes from
// TextPopupActivity (shared with the footnote popup).
class DictionaryPopupActivity final : public TextPopupActivity {
 public:
  explicit DictionaryPopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                                   int marginLeft, int marginTop, std::string lookupText = {})
      : TextPopupActivity(renderer, mappedInput, std::move(page), marginLeft, marginTop, "", "", "DictionaryPopup"),
        lookupText(std::move(lookupText)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase : uint8_t { Lookup, Show };
  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void performLookup();

  std::string lookupText;
  bool lookupPending = false;
  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Phase phase = Phase::Lookup;
  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
};
