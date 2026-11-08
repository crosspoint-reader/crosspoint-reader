#include "LocalSendReceiveActivity.h"

#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/BmpViewerActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/FirmwareBoardTag.h"
#include "util/BookCacheUtils.h"
#include "util/PluginLocations.h"

namespace {
namespace fui = freeink::ui;
constexpr const char* PLUGIN_NAME = "localsend";
constexpr const char* STATE_PATH = "/.crosspoint/localsend/localsend.json";
constexpr const char* STATE_TMP = "/.crosspoint/localsend/localsend.json.tmp";
constexpr const char* STATE_BACKUP = "/.crosspoint/localsend/localsend.json.bak";
constexpr size_t MAX_STATE_BYTES = 4096;

bool readDefinition(JsonDocument& doc) {
  const std::string dir = PluginLocations::findPluginDir(PLUGIN_NAME);
  if (dir.empty() || !Storage.exists((dir + "/manifest.json").c_str())) return false;
  std::string raw;
  if (!Storage.readFileToString("LSEND", dir + "/manifest.json", 1024, raw) ||
      deserializeJson(doc, raw, DeserializationOption::NestingLimit(4)) || doc["name"] != PLUGIN_NAME)
    return false;
  raw.clear();
  doc.clear();
  if (!Storage.readFileToString("LSEND", dir + "/device.json", 4096, raw) ||
      deserializeJson(doc, raw, DeserializationOption::NestingLimit(6)))
    return false;
  return doc["native"]["type"] == "localsend.v1" && doc["native"]["capability"] == "transfer.localsend.v1";
}

bool readState(JsonDocument& doc, const char* path) {
  std::string raw;
  doc.clear();
  return Storage.exists(path) && Storage.readFileToString("LSEND", path, MAX_STATE_BYTES, raw) &&
         !deserializeJson(doc, raw, DeserializationOption::NestingLimit(4)) && doc.is<JsonObject>();
}
}  // namespace

bool LocalSendReceiveActivity::isAvailable() {
  JsonDocument doc;
  if (!readDefinition(doc)) return false;
  const JsonArrayConst folders = doc["native"]["folders"].as<JsonArrayConst>();
  if (folders.isNull() || folders.size() == 0 || folders.size() > MAX_FOLDERS) return false;
  const char* paths[MAX_FOLDERS]{};
  int count = 0;
  for (JsonVariantConst entry : folders) {
    const char* path = entry["path"].as<const char*>();
    const char* label = entry["title"].as<const char*>();
    if (!localsend::safeDestinationFolder(path) || !label || !*label || std::strlen(label) > 48) return false;
    for (int i = 0; i < count; ++i)
      if (!std::strcmp(paths[i], path)) return false;
    paths[count++] = path;
  }
  return true;
}

LocalSendReceiveActivity::LocalSendReceiveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LocalSendReceive", renderer, mappedInput) {}

bool LocalSendReceiveActivity::loadDefinition() {
  folderCount = 0;
  JsonDocument doc;
  if (!readDefinition(doc)) return false;
  const JsonArrayConst folders = doc["native"]["folders"].as<JsonArrayConst>();
  if (folders.isNull() || folders.size() == 0 || folders.size() > MAX_FOLDERS) return false;
  for (JsonVariantConst entry : folders) {
    const char* path = entry["path"].as<const char*>();
    const char* label = entry["title"].as<const char*>();
    if (!localsend::safeDestinationFolder(path) || !label || !*label || std::strlen(label) > 48) return false;
    for (int i = 0; i < folderCount; ++i)
      if (!std::strcmp(allowedFolders[i], path)) return false;
    std::snprintf(allowedFolders[folderCount++], sizeof(allowedFolders[0]), "%s", path);
  }
  // The default must be one of this plugin's declared destinations.
  if (std::none_of(allowedFolders, allowedFolders + folderCount,
                   [this](const char* path) { return !std::strcmp(path, destination); }))
    std::snprintf(destination, sizeof(destination), "%s", allowedFolders[0]);
  return true;
}

bool LocalSendReceiveActivity::loadPreferences() {
  JsonDocument doc;
  if (!readState(doc, STATE_PATH)) {
    if (!readState(doc, STATE_BACKUP)) return !Storage.exists(STATE_PATH) && !Storage.exists(STATE_BACKUP);
    // A valid backup is authoritative only when the primary file is unusable.
    if ((!Storage.exists(STATE_PATH) || Storage.remove(STATE_PATH)) && !Storage.rename(STATE_BACKUP, STATE_PATH))
      LOG_ERR("LSEND", "Failed to restore Local Send settings backup");
  }
  const char* id = doc["device_id"].as<const char*>();
  if (LocalSendReceiver::validDeviceId(id)) std::snprintf(deviceId, sizeof(deviceId), "%s", id);
  const char* savedFingerprint = doc["fingerprint"].as<const char*>();
  if (LocalSendReceiver::validFingerprint(savedFingerprint))
    std::memcpy(fingerprint, savedFingerprint, sizeof(fingerprint));
  const JsonString selected = doc["destination"].as<JsonString>();
  if (selected && selected.size() == std::strlen(selected.c_str())) {
    for (int i = 0; i < folderCount; ++i) {
      if (!std::strcmp(selected.c_str(), allowedFolders[i])) {
        std::snprintf(destination, sizeof(destination), "%s", allowedFolders[i]);
        break;
      }
    }
  }
  return true;
}

bool LocalSendReceiveActivity::saveFingerprint() {
  if (!LocalSendReceiver::validFingerprint(fingerprint)) return false;
  JsonDocument doc;
  if (Storage.exists(STATE_PATH) && !readState(doc, STATE_PATH)) return false;
  // Preserve WebUI-owned options (including unknown future keys).
  doc["fingerprint"] = fingerprint;
  doc["destination"] = destination;
  std::string raw;
  serializeJson(doc, raw);
  if (doc.overflowed() || raw.size() > MAX_STATE_BYTES || !Storage.ensureDirectoryExists("/.crosspoint") ||
      !Storage.ensureDirectoryExists("/.crosspoint/localsend"))
    return false;
  HalFile file;
  if (!Storage.openFileForWrite("LSEND", STATE_TMP, file)) return false;
  const bool written = file.write(raw.data(), raw.size()) == raw.size();
  file.flush();
  const bool closed = file.close();
  if (!written || !closed) {
    Storage.remove(STATE_TMP);
    return false;
  }
  const bool hadOld = Storage.exists(STATE_PATH);
  if (hadOld &&
      ((Storage.exists(STATE_BACKUP) && !Storage.remove(STATE_BACKUP)) || !Storage.rename(STATE_PATH, STATE_BACKUP))) {
    Storage.remove(STATE_TMP);
    return false;
  }
  if (!Storage.rename(STATE_TMP, STATE_PATH)) {
    if (hadOld) Storage.rename(STATE_BACKUP, STATE_PATH);
    Storage.remove(STATE_TMP);
    return false;
  }
  if (Storage.exists(STATE_BACKUP)) Storage.remove(STATE_BACKUP);
  return true;
}

bool LocalSendReceiveActivity::cancelled(void* context) {
  auto& self = *static_cast<LocalSendReceiveActivity*>(context);
  self.mappedInput.update(true);
  if (gpio.rawInputActive()) {
    delay(6);
    self.mappedInput.update(true);
  }
  self.leaveRequested = self.leaveRequested || self.mappedInput.isPressed(MappedInputManager::Button::Back) ||
                        self.mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                        self.mappedInput.wasHomeGesture();
  return self.leaveRequested;
}

bool LocalSendReceiveActivity::startReceiver() {
  if (!receiver) return false;
  receiver->stop();
  // boardName() points inside the binary identity tag and is not NUL-terminated.
  char model[33]{};
  const size_t modelLen = board_tag::boardNameLen();
  if (modelLen == 0 || modelLen >= sizeof(model)) return false;
  std::memcpy(model, board_tag::boardName(), modelLen);
#if FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4
  // The X3/X4 firmware tag is shared; the LocalSend alias uses detected hardware.
  if (gpio.deviceIsX3()) std::snprintf(model, sizeof(model), "x3");
#endif
  listening = receiver->setAdvertisedId(deviceId[0] ? deviceId : model) && receiver->setDestination(destination) &&
              receiver->begin(&cancelled, this);
  seenSequence = 0;
  return listening;
}

void LocalSendReceiveActivity::onEnter() {
  UiListActivity::onEnter();
  if (!loadDefinition()) {
    LOG_ERR("LSEND", "Local Send plugin definition unavailable");
    finish();
    return;
  }
  if (!loadPreferences()) {
    const char* actions[] = {tr(STR_CANCEL), "Reset"};
    popup.showMessage("Invalid Local Send settings", "Reset saved settings to start receiving?", actions, 2, 0,
                      [this](int choice) {
                        if (choice != 1) {
                          finish();
                          return;
                        }
                        if ((Storage.exists(STATE_PATH) && !Storage.remove(STATE_PATH)) ||
                            (Storage.exists(STATE_BACKUP) && !Storage.remove(STATE_BACKUP))) {
                          showFileError("Unable to reset Local Send settings");
                          return;
                        }
                        initializeReceiver();
                      });
    requestUpdate();
    return;
  }
  initializeReceiver();
}

void LocalSendReceiveActivity::initializeReceiver() {
  std::snprintf(ipAddress, sizeof(ipAddress), "%s:53317", WiFi.localIP().toString().c_str());
  receiver = makeUniqueNoThrow<LocalSendReceiver>();
  if (!receiver) {
    LOG_ERR("LSEND", "Out of memory allocating Local Send receiver");
  } else {
    const bool newIdentity = !fingerprint[0];
    if (newIdentity) std::snprintf(fingerprint, sizeof(fingerprint), "%s", receiver->deviceFingerprint());
    if (!receiver->setFingerprint(fingerprint) ||
        ((newIdentity || !Storage.exists(STATE_PATH)) && !saveFingerprint())) {
      LOG_ERR("LSEND", "Cannot persist Local Send device fingerprint");
      receiver.reset();
    }
  }
  // Render a usable Receive screen before advertising the network listener.
  requestUpdateAndWait();
  if (receiver) startReceiver();
  requestUpdate();
}

void LocalSendReceiveActivity::onExit() {
  popup.dismiss();
  if (receiver) {
    receiver->stop();
    receiver.reset();
  }
  listening = false;
  UiListActivity::onExit();
}

void LocalSendReceiveActivity::recordCompleted() {
  if (!receiver || receiver->completedSequence() == seenSequence) return;
  seenSequence = receiver->completedSequence();
  const char* name = receiver->completedName();
  if (!localsend::safeReceiveName(name)) return;
  const std::string path = std::string(destination) + (std::strcmp(destination, "/") ? "/" : "") + name;
  {
    RenderLock lock(*this);
    if (sessionCount == MAX_SESSION_FILES) {
      // The oldest entry drops off the list; keep a suspended delete prompt
      // pointed at the same file, never at a different received file.
      if (deleteConfirmationEntry == 0)
        deleteConfirmationEntry = -1;
      else if (deleteConfirmationEntry > 0)
        --deleteConfirmationEntry;
      for (int i = 1; i < sessionCount; ++i) std::memcpy(sessionNames[i - 1], sessionNames[i], sizeof(sessionNames[0]));
      --sessionCount;
    }
    std::snprintf(sessionNames[sessionCount++], sizeof(sessionNames[0]), "%s", name);
    nav.followOnBuild = true;
  }
  if (library::isLibraryBookFile(path)) library::markLibraryIndexDirty();
  requestUpdate();
}

void LocalSendReceiveActivity::showFileError(const char* message) {
  const char* actions[] = {"Close"};
  consentPopup = false;
  popup.showMessage("Local Send", message, actions, 1, 0, [](int) {});
  requestUpdate();
}

void LocalSendReceiveActivity::confirmDeleteFile(const int entry) {
  if (entry < 0 || entry >= sessionCount) return;
  deleteConfirmationEntry = entry;
  const char* actions[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  consentPopup = false;
  popup.showMessage("Delete received file?", sessionNames[entry], actions, 2, 0, [this, entry](int choice) {
    deleteConfirmationEntry = -1;
    if (choice == 1) {
      pendingFileEntry = entry;
      pendingFileAction = FileAction::ConfirmDelete;
    }
  });
  requestUpdate();
}

void LocalSendReceiveActivity::showFileDetails(const int entry) {
  if (entry < 0 || entry >= sessionCount) return;
  if (receiver && receiver->hasActiveBatch()) {
    showFileError("Wait until all files finish receiving");
    return;
  }
  char message[128];
  std::snprintf(message, sizeof(message), "Saved in: %s", destination);
  const char* actions[] = {tr(STR_OPEN), tr(STR_DELETE), "Close"};
  consentPopup = false;
  popup.showMessage(sessionNames[entry], message, actions, 3, 0, [this, entry](int choice) {
    if (choice == 0 || choice == 1) {
      pendingFileEntry = entry;
      pendingFileAction = choice == 0 ? FileAction::Open : FileAction::Delete;
    }
  });
  requestUpdate();
}

void LocalSendReceiveActivity::handleFileAction(const FileAction action, const int entry) {
  if (receiver && receiver->hasActiveBatch()) {
    showFileError("Wait until all files finish receiving");
    return;
  }
  if (entry < 0 || entry >= sessionCount || !localsend::safeReceiveName(sessionNames[entry])) {
    showFileError("Received file not found");
    return;
  }
  const std::string path = std::string(destination) + (std::strcmp(destination, "/") ? "/" : "") + sessionNames[entry];
  auto file = Storage.open(path.c_str());
  if (!file || file.isDirectory()) {
    if (file) file.close();
    showFileError("Received file not found");
    return;
  }
  file.close();

  if (action == FileAction::Delete) {
    confirmDeleteFile(entry);
    return;
  }

  if (action == FileAction::ConfirmDelete) {
    const bool book = FsHelpers::hasEpubExtension(path);
    const bool removed = book ? removeBookFile(path) : Storage.remove(path.c_str());
    if (Storage.exists(path.c_str())) {
      showFileError("Unable to delete received file");
      return;
    }
    if (!removed) LOG_ERR("LSEND", "File deleted, but cleanup failed: %s", path.c_str());
    if (book) RECENT_BOOKS.removeByPath(path);
    if (library::isLibraryBookFile(path)) library::markLibraryIndexDirty();
    {
      RenderLock lock(*this);
      for (int i = entry + 1; i < sessionCount; ++i)
        std::memcpy(sessionNames[i - 1], sessionNames[i], sizeof(sessionNames[0]));
      --sessionCount;
      sessionNames[sessionCount][0] = '\0';
      nav.selected = std::min(nav.selected.load(), sessionCount);
      nav.followOnBuild = true;
    }
    requestUpdate();
    return;
  }

  if (action != FileAction::Open) return;
  if (FsHelpers::hasImageExtension(path)) {
    auto viewer = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, path, true);
    if (!viewer) {
      showFileError("Unable to open image viewer");
      return;
    }
    if (receiver) receiver->stop();
    listening = false;
    startActivityForResult(std::move(viewer), [this](const ActivityResult&) {
      if (receiver && WiFi.status() == WL_CONNECTED) startReceiver();
      requestUpdate();
    });
  } else if (FsHelpers::hasEpubExtension(path)) {
    if (receiver) receiver->stop();
    listening = false;
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    onSelectBook(path);
  } else {
    showFileError("Unsupported received file");
  }
}

void LocalSendReceiveActivity::showConsent() {
  const auto* offer = receiver ? receiver->pendingOffer() : nullptr;
  if (!offer) return;
  char message[320], first[140];
  const auto peer = IPAddress(offer->peer).toString();
  uint64_t total = 0;
  for (uint8_t i = 0; i < offer->fileCount; ++i) total += offer->files[i].expected;
  if (offer->fileCount == 1)
    std::snprintf(first, sizeof(first), "%.120s", offer->files[0].fileName);
  else
    std::snprintf(first, sizeof(first), "%u files (first: %.90s)", unsigned(offer->fileCount),
                  offer->files[0].fileName);
  std::snprintf(message, sizeof(message), "Receive %s (%lu KB) from %s?", first,
                static_cast<unsigned long>((total + 1023u) / 1024u), peer.c_str());
  const char* actions[] = {"Decline", "Accept"};
  confirmHeld = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  consentPopup = true;
  popup.showMessage("Local Send", message, actions, 2, 0, [this](int choice) {
    if (choice == 1) {
      // Paint an actual waiting message before the potentially slow SD free-cluster scan.
      checkingStorage.store(true);
      requestUpdateAndWait();
      if (receiver) receiver->decide(true);
      checkingStorage.store(false);
    } else if (receiver) {
      receiver->decide(false);  // Decline must never run an SD capacity scan.
    }
  });
  requestUpdate();
}

bool LocalSendReceiveActivity::handleCustomInput() {
  if (!popup.isActive()) return false;
  if (consentPopup && receiver) {
    receiver->pollConsent();
    if (!receiver->pendingOffer()) {
      popup.dismiss();
      consentPopup = false;
      requestUpdate();
      return true;
    }
  }
  // Ignore the release of the button that was held when the offer appeared.
  if (confirmHeld) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) confirmHeld = false;
    return true;
  }
  popup.handleInput(mappedInput, [this] { requestUpdate(); });
  if (!popup.isActive()) {
    if (consentPopup) {
      if (receiver && receiver->pendingOffer()) receiver->decide(false);
      consentPopup = false;
    } else {
      // An ordinary delete confirmation was dismissed rather than interrupted by consent.
      deleteConfirmationEntry = -1;
    }
  }
  return true;
}

void LocalSendReceiveActivity::onBackButton() {
  leaveRequested = true;
  finish();
}

void LocalSendReceiveActivity::loop() {
  if (leaveRequested) {
    finish();
    return;
  }
  UiListActivity::loop();
  if (pendingFileAction != FileAction::None) {
    const FileAction action = pendingFileAction;
    const int entry = pendingFileEntry;
    pendingFileAction = FileAction::None;
    pendingFileEntry = -1;
    handleFileAction(action, entry);
    return;
  }
  // Keep accepting requests while a file-details or delete-confirmation popup is open.
  // Sender consent also keeps the listener responsive to additional peers.
  if (leaveRequested || !receiver) return;
  if (WiFi.status() != WL_CONNECTED) {
    restartOnReconnect = true;
    if (listening) {
      receiver->stop();
      listening = false;
      requestUpdate();
    }
  } else {
    if (restartOnReconnect) {
      // Restart once when Wi-Fi returns; don't retry a failed bind on every UI loop.
      restartOnReconnect = false;
      if (!listening && startReceiver()) {
        std::snprintf(ipAddress, sizeof(ipAddress), "%s:53317", WiFi.localIP().toString().c_str());
        requestUpdate();
      }
    }
    if (listening) {
      receiver->poll();
      if (leaveRequested) {
        finish();
        return;
      }
      recordCompleted();
    }
  }
  // A stopped receiver must still release its consent popup and restore a
  // suspended delete confirmation.
  if (consentPopup && !receiver->pendingOffer()) {
    popup.dismiss();
    consentPopup = false;
    requestUpdate();
  } else if (listening && receiver->pendingOffer() && !consentPopup) {
    if (popup.isActive()) popup.dismiss();
    showConsent();
  }
  // Consent temporarily replaces a delete confirmation. Restore it only after
  // the offer ends and there is no active transfer that would block deletion.
  if (deleteConfirmationEntry >= 0 && !consentPopup && !popup.isActive() && !receiver->pendingOffer() &&
      !receiver->hasActiveBatch()) {
    const int entry = deleteConfirmationEntry;
    deleteConfirmationEntry = -1;
    confirmDeleteFile(entry);
  }
}

int LocalSendReceiveActivity::listCount() const { return 1 + sessionCount; }

void LocalSendReceiveActivity::activateIndex(const int index) {
  if (index <= 0 || index >= listCount() || popup.isActive()) return;
  app.clearTapFlash();
  showFileDetails(sessionCount - index);
}

void LocalSendReceiveActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  rows[0] = {};
  rows[0].label = listening ? "Listening for files" : "Not listening";
  rows[0].subtitle = listening ? ipAddress : "Connect to Wi-Fi and reopen Local Send";
  rows[0].enabled = false;
  for (int i = 0; i < sessionCount; ++i) {
    rows[i + 1] = {};
    rows[i + 1].label = sessionNames[sessionCount - 1 - i];
    rows[i + 1].subtitle = destination;
    rows[i + 1].actionValue = static_cast<int16_t>(i + 1);
    rows[i + 1].enabled = true;
  }
  fui::ListProps props;
  props.items = rows;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 3;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void LocalSendReceiveActivity::renderOverlay() {
  if (checkingStorage.load()) {
    // This notice has no choices; draw it once in the normal e-ink frame.
    fui::GfxRendererTarget target = makeUiTarget(renderer);
    const fui::ThemeTokens& theme = refreshSharedUiThemeTokens(target);
    const fui::DeviceContext device = target.deviceContext();
    fui::InteractionBuffer<1> interactions;
    const fui::InputSnapshot noInput{};
    fui::Frame<1> frame(target, device, noInput, interactions);
    fui::PopupProps props;
    props.message = "Checking SD card space... Transfer starts after verification. This may take a while.";
    props.text = theme.bodyText;
    props.text.align = fui::TextAlign::Center;
    props.text.maxLines = 5;
    props.styles = theme.popup;
    const fui::Rect bounds = device.screen();
    const int16_t width =
        static_cast<int16_t>(std::max<int>(1, std::min<int>(bounds.width * 3 / 4, bounds.width - 16)));
    const int16_t textWidth = static_cast<int16_t>(std::max<int>(1, width - props.padding.left - props.padding.right));
    const fui::Size textSize = fui::measureWrappedText(target, props.message, props.text, textWidth);
    const int16_t height = static_cast<int16_t>(std::max<int>(
        1, std::min<int>(bounds.height - 16, textSize.height + props.padding.top + props.padding.bottom)));
    fui::popup(frame, fui::centeredRect(bounds, fui::Size{width, height}), props);
  } else if (popup.isActive()) {
    popup.render(renderer);
  }
}
#endif
