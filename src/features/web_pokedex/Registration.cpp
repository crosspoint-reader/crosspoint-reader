#include "features/web_pokedex/Registration.h"

#include <FeatureFlags.h>
#include <Logging.h>
#include <WebServer.h>

#include "core/features/FeatureCatalog.h"
#include "core/registries/WebRouteRegistry.h"
#include "network/WebUtils.h"
#include "network/html/PokedexPluginPageHtml.generated.h"

namespace features::web_pokedex {
namespace {

#if ENABLE_WEB_POKEDEX_PLUGIN
bool shouldRegisterPokedexPluginRoute() { return core::FeatureCatalog::isEnabled("web_pokedex_plugin"); }

void mountPokedexRoutes(WebServer* server) {
  server->on("/plugins/pokedex", HTTP_GET, [server] {
    sendPrecompressedHtml(server, PokedexPluginPageHtml, PokedexPluginPageHtmlCompressedSize);
    LOG_DBG("WEB", "Served pokedex plugin page");
  });
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_WEB_POKEDEX_PLUGIN
  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "pokedex_plugin";
  webRouteEntry.shouldRegister = shouldRegisterPokedexPluginRoute;
  webRouteEntry.mountRoutes = mountPokedexRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::web_pokedex
