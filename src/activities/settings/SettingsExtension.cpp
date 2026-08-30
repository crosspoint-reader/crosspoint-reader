#include "SettingsExtension.h"

#include <utility>

namespace {
SettingsExtensionProvider gProvider = nullptr;
}  // namespace

void setSettingsExtensionProvider(SettingsExtensionProvider provider) { gProvider = provider; }

SettingsExtensionProvider getSettingsExtensionProvider() { return gProvider; }

void runAfterExtensionAction(ActivityResultHandler& installed, std::function<void()> followUp) {
  if (!followUp) return;
  if (!installed) {
    // The action opened no child screen, so its effects are already visible.
    followUp();
    return;
  }
  installed = [inner = std::move(installed), followUp = std::move(followUp)](const ActivityResult& result) {
    inner(result);
    followUp();
  };
}
