#include "core/CoreBootstrap.h"

#include <Logging.h>

#include "core/features/FeatureCatalog.h"
#include "features/anki/Registration.h"
#include "features/ble_wifi_provisioning/Registration.h"
#include "features/calibre_sync/Registration.h"
#include "features/dark_mode/Registration.h"
#include "features/epub/Registration.h"
#include "features/koreader_sync/Registration.h"
#include "features/lyra_theme/Registration.h"
#include "features/markdown/Registration.h"
#include "features/ota_updates/Registration.h"
#include "features/pokemon_party/Registration.h"
#include "features/remote_keyboard_input/Registration.h"
#include "features/todo_planner/Registration.h"
#include "features/trmnl_switch/Registration.h"
#include "features/txt/Registration.h"
#include "features/usb_mass_storage/Registration.h"
#include "features/user_fonts/Registration.h"
#include "features/status_overlay/Registration.h"
#include "features/visual_cover_picker/Registration.h"
#include "features/web_pokedex/Registration.h"
#include "features/web_wallpaper/Registration.h"
#include "features/web_wifi_setup/Registration.h"
#include "features/xtc/Registration.h"

namespace core {
namespace {
FeatureSystemStatus gFeatureStatus{false, false, ""};
bool gFeatureRegistrationsInitialized = false;

void registerFeatureModules() {
  if (gFeatureRegistrationsInitialized) {
    return;
  }

  features::dark_mode::registerFeature();
  features::user_fonts::registerFeature();
  features::anki::registerFeature();
  features::koreader_sync::registerFeature();

  features::epub::registerFeature();
  features::xtc::registerFeature();
  features::markdown::registerFeature();
  features::txt::registerFeature();

  features::calibre_sync::registerFeature();
  features::ota_updates::registerFeature();
  features::todo_planner::registerFeature();
  features::pokemon_party::registerFeature();
  features::remote_keyboard_input::registerFeature();
  features::web_pokedex::registerFeature();
  features::web_wallpaper::registerFeature();
  features::web_wifi_setup::registerFeature();
  features::lyra_theme::registerFeature();
  features::trmnl_switch::registerFeature();
  features::ble_wifi_provisioning::registerFeature();
  features::usb_mass_storage::registerFeature();
  features::visual_cover_picker::registerFeature();
  features::status_overlay::registerFeature();

  gFeatureRegistrationsInitialized = true;
}
}  // namespace

void CoreBootstrap::initializeFeatureSystem(const bool logSummary) {
  registerFeatureModules();

  gFeatureStatus.initialized = true;
  gFeatureStatus.validationError = "";
  gFeatureStatus.dependencyGraphValid = FeatureCatalog::validate(&gFeatureStatus.validationError);

  if (!gFeatureStatus.dependencyGraphValid) {
    LOG_ERR("CORE", "Feature dependency validation failed: %s", gFeatureStatus.validationError.c_str());
  } else if (logSummary) {
    LOG_INF("CORE", "Feature dependency validation passed");
  }

  if (logSummary) {
    FeatureCatalog::printToSerial();
  }
}

const FeatureSystemStatus& CoreBootstrap::getFeatureSystemStatus() { return gFeatureStatus; }

}  // namespace core
