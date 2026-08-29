#include "KeyboardExtension.h"

namespace {
KeyboardExtensionLayerProvider gLayerProvider = nullptr;
KeyboardExtensionDefaultPredicate gDefaultPredicate = nullptr;
KeyboardExtensionLabelProvider gLabelProvider = nullptr;
}  // namespace

void setKeyboardExtensionLayerProvider(KeyboardExtensionLayerProvider provider) { gLayerProvider = provider; }

KeyboardExtensionLayerProvider getKeyboardExtensionLayerProvider() { return gLayerProvider; }

void setKeyboardExtensionDefaultPredicate(KeyboardExtensionDefaultPredicate predicate) {
  gDefaultPredicate = predicate;
}

KeyboardExtensionDefaultPredicate getKeyboardExtensionDefaultPredicate() { return gDefaultPredicate; }

void setKeyboardExtensionLabelProvider(KeyboardExtensionLabelProvider provider) { gLabelProvider = provider; }

KeyboardExtensionLabelProvider getKeyboardExtensionLabelProvider() { return gLabelProvider; }
