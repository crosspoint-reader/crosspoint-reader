#include "features/txt/Registration.h"

#include <HalStorage.h>
#include <Logging.h>

#include <memory>
#include <string>

#include "SpiBusMutex.h"
#include "Txt.h"
#include "activities/reader/TxtReaderActivity.h"
#include "core/registries/ReaderRegistry.h"

namespace features::txt {
namespace {

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

  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint"));
  if (!txt->load()) {
    LOG_ERR("READER", "Failed to load TXT");
    return nullptr;
  }

  return new TxtReaderActivity(renderer, mappedInput, std::move(txt));
}

}  // namespace

void registerFeature() {
  core::ReaderEntry entry{
      ".txt", nullptr, nullptr, nullptr, createActivity,
  };
  core::ReaderRegistry::add(entry);
}

}  // namespace features::txt
