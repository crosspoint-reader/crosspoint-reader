#include "DictionarySearchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "activities/reader/DictionaryWordSelectActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace {
constexpr size_t MAX_QUERY_BYTES = 64;
constexpr unsigned long MESSAGE_DURATION_MS = 1500;
}  // namespace

void DictionarySearchActivity::onEnter() {
  Activity::onEnter();
  if (SETTINGS.dictionaryName[0] == '\0') {
    noDictionary = true;
    noDictionaryTime = millis();
    requestUpdate();
    return;
  }
  openKeyboard();
}

void DictionarySearchActivity::openKeyboard() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DICTIONARY), lastWord,
                                                           MAX_QUERY_BYTES, InputType::Text);
  if (!keyboard) {
    LOG_ERR("DSRCH", "OOM: keyboard");
    activityManager.goHome(HomeMenuItem::DICTIONARY);
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      activityManager.goHome(HomeMenuItem::DICTIONARY);
      return;
    }
    lastWord = std::get<KeyboardResult>(result.data).text;
    lookUp();
  });
}

void DictionarySearchActivity::lookUp() {
  // No background page: the lookup screen shows only its status popups.
  auto lookup = makeUniqueNoThrow<DictionaryWordSelectActivity>(renderer, mappedInput, nullptr, 0, 0, lastWord,
                                                                VocabularyContext{}, false);
  if (!lookup) {
    LOG_ERR("DSRCH", "OOM: dictionary lookup");
    activityManager.goHome(HomeMenuItem::DICTIONARY);
    return;
  }
  // A miss returns normally; a long Back on the definition or a lookup error
  // returns cancelled, which leaves search as it leaves an in-book lookup.
  startActivityForResult(std::move(lookup), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      activityManager.goHome(HomeMenuItem::DICTIONARY);
      return;
    }
    openKeyboard();
  });
}

void DictionarySearchActivity::loop() {
  if (noDictionary && millis() - noDictionaryTime >= MESSAGE_DURATION_MS) {
    activityManager.goHome(HomeMenuItem::DICTIONARY);
  }
}

void DictionarySearchActivity::render(RenderLock&&) {
  if (!noDictionary) return;
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
}
