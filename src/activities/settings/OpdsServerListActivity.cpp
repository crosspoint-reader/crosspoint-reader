#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

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

fui::ListItem makeRow(const char* label, const char* subtitle, const int actionValue) {
  fui::ListItem item;
  item.label = label;
  item.subtitle = subtitle;
  item.actionValue = static_cast<int16_t>(actionValue);
  return item;
}
}  // namespace

OpdsServerListActivity::OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const bool pickerMode, const OpdsBackTo backTo, std::string initialQuery)
    : UiListActivity("OpdsServerList", renderer, mappedInput),
      pickerMode(pickerMode),
      backTo(backTo),
      initialQuery_(std::move(initialQuery)) {}

int OpdsServerListActivity::serverRows() const {
  // The store never holds more than MAX_SERVERS (loading and adding both stop there); the clamp
  // keeps rowItems_ in bounds whatever opds.json says.
  return static_cast<int>(std::min(OPDS_STORE.getCount(), OpdsServerStore::MAX_SERVERS));
}

int OpdsServerListActivity::getItemCount() const {
  // Picker mode lists the servers only; settings mode adds its three rows after them.
  return serverRows() + (pickerMode ? 0 : SETTINGS_ROWS);
}

void OpdsServerListActivity::onEnter() {
  UiListActivity::onEnter();
  // Reload from disk in case servers were added/removed by a subactivity or the web UI.
  reloadServers();
  // The base onEnter() already asked for a render, possibly built before this reload.
  requestUpdate();
}

void OpdsServerListActivity::reloadServers() {
  // The rows point into the store's strings, and a reload replaces every one of them: it must not
  // overlap a build.
  RenderLock lock(*this);
  OPDS_STORE.loadFromFile();
}

// Fills rowItems_ from OPDS_STORE and SETTINGS; returns the row count. Called by buildScreen() on
// the render task, which holds the render lock for the whole pass, so the store cannot reload under
// it. Pointer assignments only: nothing is copied or allocated.
int OpdsServerListActivity::rebuildRowItems() {
  const auto& servers = OPDS_STORE.getServers();
  const int serverCount = serverRows();
  int count = 0;
  for (int i = 0; i < serverCount; i++) {
    const OpdsServer& server = servers[static_cast<size_t>(i)];
    // Primary label: server name (falling back to URL if unnamed).
    // Subtitle: the URL, only when the name is set.
    rowItems_[count++] = makeRow(server.name.empty() ? server.url.c_str() : server.name.c_str(),
                                 server.name.empty() ? nullptr : server.url.c_str(), i);
  }
  if (pickerMode) return count;

  rowItems_[count++] = makeRow(tr(STR_ADD_SERVER), nullptr, serverCount);
  rowItems_[count++] =
      makeRow(tr(STR_OPDS_DOWNLOAD_FOLDER),
              SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT), serverCount + 1);
  rowItems_[count++] =
      makeRow(tr(STR_OPDS_FILENAME_FORMAT), I18N.get(opdsFormatLabel(SETTINGS.opdsFilenameFormat)), serverCount + 2);
  return count;
}

void OpdsServerListActivity::onBackButton() {
  if (!pickerMode) {
    finish();
    return;
  }
  switch (backTo) {
    case OpdsBackTo::FileTransfer:
      activityManager.goToFileTransfer(NetworkMode::OPDS_BROWSER);
      return;
    case OpdsBackTo::Home:
      activityManager.goHome();
      return;
  }
}

void OpdsServerListActivity::homeFromList() {
  if (pickerMode) {
    activityManager.goHome();
  } else {
    UiListActivity::homeFromList();
  }
}

const char* OpdsServerListActivity::headerTitle() const { return tr(STR_OPDS_SERVERS); }

void OpdsServerListActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens an editor/browser or repaints a new value; a lingering
  // flash would gray an unrelated row.
  app.clearTapFlash();
  handleSelection(index);
  requestUpdate();
}

void OpdsServerListActivity::handleSelection(const int index) {
  const int serverCount = serverRows();

  if (pickerMode) {
    // Picker mode: selecting a server navigates to the OPDS browser
    if (index < serverCount) {
      const auto* server = OPDS_STORE.getServer(static_cast<size_t>(index));
      if (server) {
        activityManager.replaceActivity(
            std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, *server, backTo, initialQuery_));
      }
    }
    return;
  }

  // Index layout: [servers 0..serverCount-1], [Add Server], [Download folder], [Filename format].
  if (index == serverCount + 1) {
    auto folderHandler = [this](const ActivityResult& result) {
      if (result.isCancelled) return;
      const auto& kb = std::get<KeyboardResult>(result.data);
      const std::string norm = normalizeFolder(kb.text);
      {
        // The folder row's subtitle points at this buffer while a build draws it.
        RenderLock lock(*this);
        strncpy(SETTINGS.opdsDownloadFolder, norm.c_str(), sizeof(SETTINGS.opdsDownloadFolder) - 1);
        SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
      }
      SETTINGS.saveToFile();
      requestUpdate();
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER),
                                                std::string(SETTINGS.opdsDownloadFolder), 63, InputType::Text),
        folderHandler);
    return;
  }

  // "Filename format": Confirm cycles through the available formats.
  if (index == serverCount + 2) {
    {
      // The format row's subtitle is chosen from this value while a build draws it.
      RenderLock lock(*this);
      SETTINGS.opdsFilenameFormat =
          static_cast<uint8_t>((SETTINGS.opdsFilenameFormat + 1) % static_cast<uint8_t>(OpdsFilenameFormat::Count));
    }
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  // A server row opens its editor; "Add Server" opens the editor on a new server.
  auto resultHandler = [this](const ActivityResult&) {
    // The editor saved (or deleted) on its own. Reload, and keep the user's place, clamped in case
    // the server under the selection was deleted.
    reloadServers();
    const int itemCount = getItemCount();
    moveSelectionTo(itemCount > 0 ? std::min(nav.selected.load(), itemCount - 1) : 0);
  };
  startActivityForResult(
      std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, index < serverCount ? index : -1), resultHandler);
}

void OpdsServerListActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);

  const int count = rebuildRowItems();
  if (count == 0) {
    screen.centeredText(tr(STR_NO_SERVERS), screen.theme().bodyText);
    return;
  }

  auto props = listProps(screen);
  // Like the old screen, the labels stay on one line: the subtitle row height is fixed.
  props.labelText = {};
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(count);
  addList(screen, props, /*hasSubtitle=*/true);
}
