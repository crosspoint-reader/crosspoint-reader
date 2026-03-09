#include "features/koreader_sync/Registration.h"

#include <FeatureFlags.h>
#include <GfxRenderer.h>

#include <cstring>

#include "KOReaderCredentialStore.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "core/features/FeatureCatalog.h"
#include "core/registries/LifecycleRegistry.h"
#include "core/registries/SettingsActionRegistry.h"
#include "core/registries/SyncServiceRegistry.h"

namespace features::koreader_sync {
namespace {

void copyToBuffer(const std::string& value, char* outBuf, const size_t bufSize) {
  if (outBuf == nullptr || bufSize == 0) {
    return;
  }

  std::strncpy(outBuf, value.c_str(), bufSize - 1);
  outBuf[bufSize - 1] = '\0';
}

void onSettingsLoaded(GfxRenderer& renderer) {
  (void)renderer;
  KOREADER_STORE.loadFromFile();
}

bool hasCredentials() { return KOREADER_STORE.hasCredentials(); }

void getUsername(char* outBuf, size_t bufSize) { copyToBuffer(KOREADER_STORE.getUsername(), outBuf, bufSize); }

void getPassword(char* outBuf, size_t bufSize) { copyToBuffer(KOREADER_STORE.getPassword(), outBuf, bufSize); }

void getServerUrl(char* outBuf, size_t bufSize) { copyToBuffer(KOREADER_STORE.getServerUrl(), outBuf, bufSize); }

uint8_t getMatchMethod() { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); }

void setUsername(const char* value, const bool save) {
  KOREADER_STORE.setCredentials(value != nullptr ? value : "", KOREADER_STORE.getPassword());
  if (save) {
    KOREADER_STORE.saveToFile();
  }
}

void setPassword(const char* value, const bool save) {
  KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), value != nullptr ? value : "");
  if (save) {
    KOREADER_STORE.saveToFile();
  }
}

void setServerUrl(const char* value, const bool save) {
  KOREADER_STORE.setServerUrl(value != nullptr ? value : "");
  if (save) {
    KOREADER_STORE.saveToFile();
  }
}

void setMatchMethod(const uint8_t method, const bool save) {
  const auto selectedMethod = method == static_cast<uint8_t>(DocumentMatchMethod::BINARY)
                                  ? DocumentMatchMethod::BINARY
                                  : DocumentMatchMethod::FILENAME;
  KOREADER_STORE.setMatchMethod(selectedMethod);
  if (save) {
    KOREADER_STORE.saveToFile();
  }
}

void saveSettings() { KOREADER_STORE.saveToFile(); }

bool isSettingsActionSupported() { return core::FeatureCatalog::isEnabled("koreader_sync"); }

Activity* createSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, void* callbackCtx,
                                 void (*onComplete)(void* ctx), void (*onCompleteBool)(void* ctx, bool result)) {
  (void)callbackCtx;
  (void)onComplete;
  (void)onCompleteBool;
  return new KOReaderSettingsActivity(renderer, mappedInput);
}

}  // namespace

void registerFeature() {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  core::LifecycleEntry lifecycleEntry{};
  lifecycleEntry.onSettingsLoaded = onSettingsLoaded;
  core::LifecycleRegistry::add(lifecycleEntry);

  core::KoreaderServiceApi api{};
  api.hasCredentials = hasCredentials;
  api.getUsername = getUsername;
  api.getPassword = getPassword;
  api.getServerUrl = getServerUrl;
  api.getMatchMethod = getMatchMethod;
  api.setUsername = setUsername;
  api.setPassword = setPassword;
  api.setServerUrl = setServerUrl;
  api.setMatchMethod = setMatchMethod;
  api.saveSettings = saveSettings;
  core::SyncServiceRegistry::setKoreaderApi(api);

  core::SettingsActionEntry settingsEntry{};
  settingsEntry.action = SettingAction::KOReaderSync;
  settingsEntry.isSupported = isSettingsActionSupported;
  settingsEntry.create = createSettingsActivity;
  core::SettingsActionRegistry::add(settingsEntry);
#endif
}

}  // namespace features::koreader_sync
