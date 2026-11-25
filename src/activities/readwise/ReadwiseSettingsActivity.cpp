#include "ReadwiseSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>

#include "MappedInputManager.h"
#include "ReadwiseSyncActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "readwise/ReadwiseStore.h"

namespace fui = freeink::ui;

namespace {
constexpr int IDX_SYNC = 0;
constexpr int IDX_TOKEN = 1;
constexpr int IDX_SOURCES = 2;
constexpr int IDX_ARCHIVE = 3;
constexpr int IDX_REMOVE = 4;
constexpr int IDX_FOLDER = 5;

constexpr StrId menuNames[ReadwiseSettingsActivity::MENU_ITEMS] = {
    StrId::STR_READWISE_SYNC_NOW,         StrId::STR_READWISE_TOKEN,           StrId::STR_READWISE_SOURCES,
    StrId::STR_READWISE_ARCHIVE_FINISHED, StrId::STR_READWISE_REMOVE_ARCHIVED, StrId::STR_READWISE_FOLDER};

constexpr int SOURCE_ITEMS = static_cast<int>(ReadwiseSources::Count);
constexpr StrId sourceNames[SOURCE_ITEMS] = {StrId::STR_READWISE_SRC_LATER, StrId::STR_READWISE_SRC_LATER_SHORTLIST,
                                             StrId::STR_READWISE_SRC_ALL};
}  // namespace

ReadwiseSettingsActivity::ReadwiseSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadwiseSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* ReadwiseSettingsActivity::headerTitle() const { return tr(STR_READWISE); }

bool ReadwiseSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ReadwiseSettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  app.clearTapFlash();
  if (index == IDX_SYNC) {
    if (!READWISE_STORE.hasToken()) return;
    startActivityForResult(std::make_unique<ReadwiseSyncActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  } else if (index == IDX_TOKEN) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READWISE_TOKEN),
                                                                   READWISE_STORE.getToken(), 128, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) return;
                             READWISE_STORE.setToken(std::get<KeyboardResult>(result.data).text);
                             READWISE_STORE.saveToFile();
                           });
  } else if (index == IDX_SOURCES) {
    optionPopup.show(StrId::STR_READWISE_SOURCES, sourceNames, SOURCE_ITEMS,
                     static_cast<int>(READWISE_STORE.getSources()), [](const int selected) {
                       READWISE_STORE.setSources(static_cast<ReadwiseSources>(selected));
                       READWISE_STORE.saveToFile();
                     });
    requestUpdate();
  } else if (index == IDX_ARCHIVE) {
    READWISE_STORE.setArchiveFinished(!READWISE_STORE.getArchiveFinished());
    READWISE_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_REMOVE) {
    READWISE_STORE.setRemoveArchived(!READWISE_STORE.getRemoveArchived());
    READWISE_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_FOLDER) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READWISE_FOLDER),
                                                                   READWISE_STORE.getFolder(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) return;
                             READWISE_STORE.setFolder(std::get<KeyboardResult>(result.data).text);
                             READWISE_STORE.saveToFile();
                           });
  }
}

void ReadwiseSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const std::string& token = READWISE_STORE.getToken();
  rowValues_[IDX_SYNC] = token.empty() ? std::string("[") + tr(STR_READWISE_SET_TOKEN_FIRST) + "]" : std::string();
  if (token.empty()) {
    rowValues_[IDX_TOKEN] = std::string(tr(STR_NOT_SET)) + " - " + tr(STR_READWISE_TOKEN_HINT);
  } else {
    rowValues_[IDX_TOKEN] = "****" + (token.size() > 4 ? token.substr(token.size() - 4) : std::string());
  }
  rowValues_[IDX_SOURCES] = I18N.get(sourceNames[static_cast<int>(READWISE_STORE.getSources())]);
  rowValues_[IDX_ARCHIVE].clear();
  rowValues_[IDX_REMOVE].clear();
  rowValues_[IDX_FOLDER] = READWISE_STORE.getFolder();
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  GUI.setCheckboxRow(rowItems_[IDX_ARCHIVE], READWISE_STORE.getArchiveFinished());
  GUI.setCheckboxRow(rowItems_[IDX_REMOVE], READWISE_STORE.getRemoveArchived());

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void ReadwiseSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
