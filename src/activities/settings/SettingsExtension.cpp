#include "SettingsExtension.h"

namespace {
SettingsExtensionProvider gProvider = nullptr;
}  // namespace

void setSettingsExtensionProvider(SettingsExtensionProvider provider) { gProvider = provider; }

SettingsExtensionProvider getSettingsExtensionProvider() { return gProvider; }
