#include "features/epub/Registration.h"

#include <Epub.h>
#include <FeatureFlags.h>
#include <HalStorage.h>
#include <Logging.h>

#include <memory>
#include <string>

#include "SpiBusMutex.h"
#include "activities/reader/EpubReaderActivity.h"
#include "core/registries/ReaderRegistry.h"

namespace features::epub {
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

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (!epub->load()) {
    LOG_ERR("READER", "Failed to load EPUB");
    return nullptr;
  }

  return new EpubReaderActivity(renderer, mappedInput, std::move(epub));
}

}  // namespace

void registerFeature() {
#if ENABLE_EPUB_SUPPORT
  core::ReaderEntry entry{
      ".epub",        isSupported, "EPUB support disabled in this build", "EPUB support\nnot available\nin this build",
      createActivity,
  };
  core::ReaderRegistry::add(entry);
#endif
}

}  // namespace features::epub
