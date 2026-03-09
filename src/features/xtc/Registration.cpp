#include "features/xtc/Registration.h"

#include <FeatureFlags.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <memory>
#include <string>

#include "SpiBusMutex.h"
#include "activities/reader/XtcReaderActivity.h"
#include "core/registries/ReaderRegistry.h"

namespace features::xtc {
namespace {

bool isSupported(const char* path) {
  (void)path;
  return true;
}

Activity* createActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& path,
                         void* callbackCtx, void (*onBackToLibrary)(void* ctx, const std::string& path),
                         void (*onBackHome)(void* ctx)) {
  (void)callbackCtx;
  (void)onBackToLibrary;
  (void)onBackHome;

  SpiBusMutex::Guard guard;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (!xtc->load()) {
    LOG_ERR("READER", "Failed to load XTC");
    return nullptr;
  }

  return new XtcReaderActivity(renderer, mappedInput, std::move(xtc));
}

void addEntry(const char* extension) {
  core::ReaderEntry entry{
      extension,      isSupported, "XTC support disabled in this build", "XTC support\nnot available\nin this build",
      createActivity,
  };
  core::ReaderRegistry::add(entry);
}

}  // namespace

void registerFeature() {
#if ENABLE_XTC_SUPPORT
  addEntry(".xtc");
  addEntry(".xtch");
#endif
}

}  // namespace features::xtc
