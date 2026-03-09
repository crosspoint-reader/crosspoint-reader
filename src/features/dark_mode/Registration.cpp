#include "features/dark_mode/Registration.h"

#include <FeatureFlags.h>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "core/registries/LifecycleRegistry.h"

namespace features::dark_mode {
namespace {

void onSettingsLoaded(GfxRenderer& renderer) { renderer.setDarkMode(SETTINGS.darkMode); }

}  // namespace

void registerFeature() {
#if ENABLE_DARK_MODE
  core::LifecycleEntry entry{};
  entry.onSettingsLoaded = onSettingsLoaded;
  core::LifecycleRegistry::add(entry);
#endif
}

}  // namespace features::dark_mode
