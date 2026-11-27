#include "DictionarySearchActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "DictionaryHistoryActivity.h"
#include "activities/reader/DictionaryWordSelectActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "components/icons/history32.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t MAX_QUERY_BYTES = 64;
constexpr unsigned long MESSAGE_DURATION_MS = 1500;
}  // namespace

const char* DictionarySearchActivity::headerTitle() const { return tr(STR_DICTIONARY); }

void DictionarySearchActivity::onEnter() {
  {
    // The base onEnter schedules a paint; hold the lock until the rows are filled.
    RenderLock lock;
    UiListActivity::onEnter();
    app.on(ACTION_HISTORY, &DictionarySearchActivity::historyActionTrampoline, this);
    if (SETTINGS.dictionaryName[0] == '\0') {
      noDictionary = true;
      noDictionaryTime = millis();
      return;
    }
    reloadRows();
  }
  // Nothing to pick from yet: go straight to typing.
  if (words.empty()) openKeyboard();
}

void DictionarySearchActivity::reloadRows() {
  dicthistory::load(words, RECENT_WORDS);
  rowItems[0].label = tr(STR_DICT_NEW_SEARCH);
  rowItems[0].actionValue = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    rowItems[i + 1].label = words[i].c_str();
    rowItems[i + 1].actionValue = static_cast<int16_t>(i + 1);
  }
}

void DictionarySearchActivity::activateIndex(const int index) {
  if (index == 0) {
    openKeyboard();
  } else if (index > 0 && index <= static_cast<int>(words.size())) {
    lookUp(words[index - 1]);
  }
}

void DictionarySearchActivity::openKeyboard() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DICTIONARY), "",
                                                           MAX_QUERY_BYTES, InputType::Text);
  if (!keyboard) {
    LOG_ERR("DSRCH", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      lookUp(std::get<KeyboardResult>(result.data).text);
    } else if (words.empty()) {
      activityManager.goHome(HomeMenuItem::DICTIONARY);
    }
  });
}

void DictionarySearchActivity::historyActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<DictionarySearchActivity*>(user)->openHistory();
}

void DictionarySearchActivity::openHistory() {
  auto history = makeUniqueNoThrow<DictionaryHistoryActivity>(renderer, mappedInput);
  if (!history) {
    LOG_ERR("DSRCH", "OOM: dictionary history");
    return;
  }
  startActivityForResult(std::move(history), [this](const ActivityResult& result) {
    const auto* picked = std::get_if<KeyboardResult>(&result.data);
    if (!result.isCancelled && picked) lookUp(picked->text, true);
  });
}

void DictionarySearchActivity::lookUp(const std::string& word, const bool reopenHistory) {
  // No background page: the lookup screen shows only its status popups.
  auto lookup = makeUniqueNoThrow<DictionaryWordSelectActivity>(
      renderer, mappedInput, nullptr, 0, 0, word, VocabularyContext{}, DictionaryWordSelectActivity::Source::Search);
  if (!lookup) {
    LOG_ERR("DSRCH", "OOM: dictionary lookup");
    return;
  }
  // A miss returns normally; a long Back on the definition or a lookup error
  // returns cancelled, which leaves search as it leaves an in-book lookup.
  startActivityForResult(std::move(lookup), [this, reopenHistory](const ActivityResult& result) {
    if (result.isCancelled) {
      activityManager.goHome(HomeMenuItem::DICTIONARY);
      return;
    }
    {
      RenderLock lock;
      reloadRows();
      moveSelectionTo(0);
    }
    if (reopenHistory) openHistory();
  });
}

void DictionarySearchActivity::loop() {
  if (noDictionary) {
    if (millis() - noDictionaryTime >= MESSAGE_DURATION_MS) activityManager.goHome(HomeMenuItem::DICTIONARY);
    return;
  }
  UiListActivity::loop();
}

void DictionarySearchActivity::render(RenderLock&& lock) {
  if (!noDictionary) {
    UiListActivity::render(std::move(lock));
    return;
  }
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
}

void DictionarySearchActivity::buildScreen(UiScreen& screen) {
  // Sets the content margin below the header and above the button hints.
  catalogScreenHeader(screen, renderer, tr(STR_DICTIONARY), fui::bitmapFromIcon(icon_history_32), ACTION_HISTORY);

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
