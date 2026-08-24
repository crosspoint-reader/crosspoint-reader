#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/OpdsFilename.h"

namespace fui = freeink::ui;

namespace {
// Normalizes a user-typed folder: trims spaces, "" => SD root, otherwise a
// single leading '/' and no trailing '/'. Cold path (runs once per edit).
std::string normalizeFolder(std::string v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
  if (v.empty()) return "";
  if (v.front() != '/') v.insert(v.begin(), '/');
  while (v.size() > 1 && v.back() == '/') v.pop_back();
  if (v == "/") return "";  // a bare slash is SD root, same as empty
  return v;
}

// Label shown for the current OPDS filename format in the list subtitle.
StrId opdsFormatLabel(uint8_t format) {
  switch (format) {
    case static_cast<uint8_t>(OpdsFilenameFormat::TitleAuthor):
      return StrId::STR_FMT_TITLE_AUTHOR;
    case static_cast<uint8_t>(OpdsFilenameFormat::TitleOnly):
      return StrId::STR_FMT_TITLE;
    default:
      return StrId::STR_FMT_AUTHOR_TITLE;
  }
}
}  // namespace

int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // Settings mode appends three virtual items: "Add Server", "Download folder"
  // and "Filename format".
  if (!pickerMode) {
    count += 3;
  }
  return count;
}

OpdsServerListActivity::OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const bool pickerMode)
    : UiListActivity("OpdsServerList", renderer, mappedInput), pickerMode(pickerMode) {}

void OpdsServerListActivity::onEnter() {
  UiListActivity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  nav.selected = 0;
  rebuildRowItems();
}

// Rebuilds rowItems_ (labels/actionValue, server subtitles) from OPDS_STORE.
// Structural — call only when the server list actually reloads, not from
// buildScreen(). The folder/format rows' live subtitle is refreshed in place
// by buildScreen() every render instead, since those track live SETTINGS
// values that can change without a server-list reload.
void OpdsServerListActivity::rebuildRowItems() {
  rowItems_.clear();
  const int itemCount = getItemCount();
  if (itemCount == 0) return;
  rowItems_.reserve(itemCount);

  const auto& servers = OPDS_STORE.getServers();
  const auto serverCount = static_cast<int>(servers.size());
  for (int i = 0; i < serverCount; i++) {
    fui::ListItem item;
    item.label = servers[i].name.empty() ? servers[i].url.c_str() : servers[i].name.c_str();
    if (!servers[i].name.empty()) item.subtitle = servers[i].url.c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
  if (!pickerMode) {
    fui::ListItem addServer;
    addServer.label = tr(STR_ADD_SERVER);
    addServer.actionValue = static_cast<int16_t>(serverCount);
    rowItems_.push_back(addServer);

    fui::ListItem folder;
    folder.label = tr(STR_OPDS_DOWNLOAD_FOLDER);
    folder.actionValue = static_cast<int16_t>(serverCount + 1);
    rowItems_.push_back(folder);  // subtitle refreshed per render below

    fui::ListItem format;
    format.label = tr(STR_OPDS_FILENAME_FORMAT);
    format.actionValue = static_cast<int16_t>(serverCount + 2);
    rowItems_.push_back(format);  // subtitle refreshed per render below
  }
}

bool OpdsServerListActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void OpdsServerListActivity::onBackButton() {
  if (pickerMode) {
    activityManager.goHome(HomeMenuItem::OPDS_BROWSER);
  } else {
    finish();
  }
}

const char* OpdsServerListActivity::headerTitle() const { return tr(STR_OPDS_SERVERS); }

void OpdsServerListActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens an editor/browser or repaints a new value; a lingering
  // flash would gray an unrelated row.
  app.clearTapFlash();
  handleSelection();
  requestUpdate();
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (pickerMode) {
    // Picker mode: selecting a server navigates to the OPDS browser
    if (nav.selected < serverCount) {
      const auto* server = OPDS_STORE.getServer(static_cast<size_t>(nav.selected));
      if (server) {
        activityManager.replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, *server));
      }
    }
    return;
  }

  // Index layout: [servers 0..serverCount-1], [Add Server], [Download folder], [Filename format].
  if (nav.selected == serverCount + 1) {
    auto folderHandler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        const std::string norm = normalizeFolder(kb.text);
        strncpy(SETTINGS.opdsDownloadFolder, norm.c_str(), sizeof(SETTINGS.opdsDownloadFolder) - 1);
        SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
        SETTINGS.saveToFile();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER),
                                                std::string(SETTINGS.opdsDownloadFolder), 63, InputType::Text),
        folderHandler);
    return;
  }

  // "Filename format": picker like every other multi-option setting.
  if (nav.selected == serverCount + 2) {
    static const StrId formatLabels[] = {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR,
                                         StrId::STR_FMT_TITLE};
    optionPopup.show(StrId::STR_OPDS_FILENAME_FORMAT, formatLabels, static_cast<int>(OpdsFilenameFormat::Count),
                     SETTINGS.opdsFilenameFormat, [this](int idx) {
                       SETTINGS.opdsFilenameFormat = static_cast<uint8_t>(idx);
                       SETTINGS.saveToFile();
                     });
    requestUpdate();
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto resultHandler = [this](const ActivityResult&) {
    // Reload server list when returning from editor
    OPDS_STORE.loadFromFile();
    nav.selected = 0;
    rebuildRowItems();
  };

  if (nav.selected < serverCount) {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, nav.selected), resultHandler);
  } else {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, -1), resultHandler);
  }
}

void OpdsServerListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints; derived
  // from the safe area so board bezel insets apply (same as LanguageSelect).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int itemCount = getItemCount();
  if (itemCount == 0) {
    screen.centeredText(tr(STR_NO_SERVERS), screen.theme().bodyText);
    return;
  }

  // rowItems_ (labels/actionValue, server subtitles) was built by
  // rebuildRowItems() when the server list last reloaded; only the
  // folder/format rows' live subtitle needs refreshing here (pointer
  // reassignment onto already-owned strings — no allocation).
  if (!pickerMode) {
    const auto serverCount = static_cast<int>(OPDS_STORE.getServers().size());
    rowItems_[serverCount + 1].subtitle =
        SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT);
    rowItems_[serverCount + 2].subtitle = I18N.get(opdsFormatLabel(SETTINGS.opdsFilenameFormat));
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void OpdsServerListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the base skeleton.
  UiListActivity::render(std::move(lock));
}
