#include "features/calibre_sync/Registration.h"

#include <FeatureFlags.h>

#if ENABLE_INTEGRATIONS && ENABLE_CALIBRE_SYNC
#include "activities/browser/OpdsBookBrowserActivity.h"
#endif
#include "activities/settings/CalibreSettingsActivity.h"
#include "core/features/FeatureCatalog.h"
#include "core/registries/HomeActionRegistry.h"
#include "core/registries/SettingsActionRegistry.h"

namespace features::calibre_sync {
namespace {

static bool isSettingsActionSupported() { return core::FeatureCatalog::isEnabled("calibre_sync"); }

static Activity* createSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, void* callbackCtx,
                                        void (*onComplete)(void* ctx), void (*onCompleteBool)(void* ctx, bool result)) {
  (void)callbackCtx;
  (void)onComplete;
  (void)onCompleteBool;
  return new CalibreSettingsActivity(renderer, mappedInput);
}

#if ENABLE_INTEGRATIONS && ENABLE_CALIBRE_SYNC
static bool shouldExposeOpdsBrowserHomeAction(core::HomeActionEntry::HomeActionContext ctx) {
  return core::FeatureCatalog::isEnabled("calibre_sync") && ctx.hasOpdsUrl;
}

static Activity* createOpdsBrowserHomeActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     void* callbackCtx, void (*onBack)(void* ctx)) {
  (void)callbackCtx;
  (void)onBack;
  return new OpdsBookBrowserActivity(renderer, mappedInput);
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_INTEGRATIONS && ENABLE_CALIBRE_SYNC
  core::SettingsActionEntry settingsEntry{};
  settingsEntry.action = SettingAction::OPDSBrowser;
  settingsEntry.isSupported = isSettingsActionSupported;
  settingsEntry.create = createSettingsActivity;
  core::SettingsActionRegistry::add(settingsEntry);

  core::HomeActionEntry homeEntry{};
  homeEntry.actionId = "opds_browser";
  homeEntry.shouldExpose = shouldExposeOpdsBrowserHomeAction;
  homeEntry.create = createOpdsBrowserHomeActionActivity;
  core::HomeActionRegistry::add(homeEntry);
#endif
}

}  // namespace features::calibre_sync
