#include "features/anki/Registration.h"

#include <FeatureFlags.h>
#include <Logging.h>
#include <WebServer.h>

#if ENABLE_ANKI_SUPPORT
#include "activities/AnkiActivity.h"
#endif
#include "core/features/FeatureCatalog.h"
#include "core/registries/HomeActionRegistry.h"
#include "core/registries/LifecycleRegistry.h"
#include "core/registries/WebRouteRegistry.h"
#include "network/WebUtils.h"
#include "network/html/AnkiPluginPageHtml.generated.h"
#include "util/AnkiStore.h"

namespace features::anki {
namespace {

#if ENABLE_ANKI_SUPPORT
static bool shouldRegisterAnkiPluginRoute() { return core::FeatureCatalog::isEnabled("anki_support"); }

static void mountAnkiRoutes(WebServer* server) {
  server->on("/plugins/anki", HTTP_GET, [server] {
    sendPrecompressedHtml(server, AnkiPluginPageHtml, AnkiPluginPageHtmlCompressedSize);
    LOG_DBG("WEB", "Served anki plugin page");
  });
  server->on("/api/anki/cards", HTTP_GET, [server] {
    // buildCardsJson holds the AnkiStore mutex internally; release before send().
    std::string json;
    util::AnkiStore::getInstance().buildCardsJson(json);
    server->send(200, "application/json", json.c_str());
  });
  server->on("/api/anki/clear", HTTP_POST, [server] {
    util::AnkiStore::getInstance().clear();
    util::AnkiStore::getInstance().save();
    server->send(200, "application/json", "{\"status\":\"ok\"}");
  });
}

static bool shouldExposeAnkiHomeAction(core::HomeActionEntry::HomeActionContext ctx) {
  (void)ctx;
  return core::FeatureCatalog::isEnabled("anki_support");
}

static Activity* createAnkiHomeActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, void* callbackCtx,
                                              void (*onBack)(void* ctx)) {
  (void)callbackCtx;
  (void)onBack;
  return new AnkiActivity(renderer, mappedInput);
}
#endif

void onStorageReady() { util::AnkiStore::getInstance().load(); }

}  // namespace

void registerFeature() {
#if ENABLE_ANKI_SUPPORT
  core::LifecycleEntry entry{};
  entry.onStorageReady = onStorageReady;
  core::LifecycleRegistry::add(entry);

  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "anki_plugin";
  webRouteEntry.shouldRegister = shouldRegisterAnkiPluginRoute;
  webRouteEntry.mountRoutes = mountAnkiRoutes;
  core::WebRouteRegistry::add(webRouteEntry);

  core::HomeActionEntry homeEntry{};
  homeEntry.actionId = "anki";
  homeEntry.shouldExpose = shouldExposeAnkiHomeAction;
  homeEntry.create = createAnkiHomeActionActivity;
  core::HomeActionRegistry::add(homeEntry);
#endif
}

}  // namespace features::anki
