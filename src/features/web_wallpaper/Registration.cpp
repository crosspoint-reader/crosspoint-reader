#include "features/web_wallpaper/Registration.h"

#include <FeatureFlags.h>
#include <Logging.h>
#include <WebServer.h>

#include "core/features/FeatureCatalog.h"
#include "core/registries/WebRouteRegistry.h"
#include "network/WebUtils.h"
#include "network/html/WallpaperPluginPageHtml.generated.h"

namespace features::web_wallpaper {
namespace {

#if ENABLE_WEB_WALLPAPER_PLUGIN
bool shouldRegisterWallpaperPluginRoute() { return core::FeatureCatalog::isEnabled("web_wallpaper_plugin"); }

void mountWallpaperRoutes(WebServer* server) {
  server->on("/plugins/wallpaper", HTTP_GET, [server] {
    sendPrecompressedHtml(server, WallpaperPluginPageHtml, WallpaperPluginPageHtmlCompressedSize);
    LOG_DBG("WEB", "Served wallpaper plugin page");
  });
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_WEB_WALLPAPER_PLUGIN
  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "wallpaper_plugin";
  webRouteEntry.shouldRegister = shouldRegisterWallpaperPluginRoute;
  webRouteEntry.mountRoutes = mountWallpaperRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::web_wallpaper
