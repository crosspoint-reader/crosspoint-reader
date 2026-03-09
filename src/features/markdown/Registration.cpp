#include "features/markdown/Registration.h"

#include <FeatureFlags.h>
#include <HalStorage.h>
#include <Logging.h>

#include <memory>
#include <string>

#include "Markdown.h"
#include "SpiBusMutex.h"
#include "activities/reader/MarkdownReaderActivity.h"
#include "core/registries/ReaderRegistry.h"

namespace features::markdown {
namespace {

bool isSupported(const char* path) {
  (void)path;
  return true;
}

Activity* createActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& path,
                         void* callbackCtx, void (*onBackToLibrary)(void* ctx, const std::string& path),
                         void (*onBackHome)(void* ctx)) {
  SpiBusMutex::Guard guard;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto markdown = std::unique_ptr<Markdown>(new Markdown(path, "/.crosspoint"));
  if (!markdown->load()) {
    LOG_ERR("READER", "Failed to load Markdown");
    return nullptr;
  }

  return new MarkdownReaderActivity(renderer, mappedInput, std::move(markdown), callbackCtx, onBackToLibrary,
                                    onBackHome);
}

}  // namespace

void registerFeature() {
#if ENABLE_MARKDOWN
  core::ReaderEntry entry{
      ".md",
      isSupported,
      "Markdown support disabled in this build",
      "Markdown support\nnot available\nin this build",
      createActivity,
  };
  core::ReaderRegistry::add(entry);
#endif
}

}  // namespace features::markdown
