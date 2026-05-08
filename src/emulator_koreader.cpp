#if CROSSPOINT_EMULATED

#include "KOReaderCredentialStore.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "fontIds.h"

KOReaderCredentialStore KOReaderCredentialStore::instance;

bool KOReaderCredentialStore::saveToFile() const { return true; }
bool KOReaderCredentialStore::loadFromFile() { return false; }
bool KOReaderCredentialStore::loadFromBinaryFile() { return false; }

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
}

std::string KOReaderCredentialStore::getMd5Password() const { return password; }
bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KOReaderCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) { serverUrl = url; }

std::string KOReaderCredentialStore::getBaseUrl() const {
  if (serverUrl.empty()) {
    return "https://sync.koreader.rocks:443";
  }
  return serverUrl.find("://") == std::string::npos ? "http://" + serverUrl : serverUrl;
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) { matchMethod = method; }

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  statusMessage = "KOReader sync is not emulated";
  requestUpdate();
}

void KOReaderSyncActivity::onExit() { Activity::onExit(); }

void KOReaderSyncActivity::loop() {
  mappedInput.update();
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 240, "KOReader sync is not emulated", true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(bool) {}
void KOReaderSyncActivity::performSync() {}
void KOReaderSyncActivity::performUpload() {}

#endif
