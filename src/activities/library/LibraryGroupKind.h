#pragma once

#include <LibraryFormat.h>

#include "CrossPointSettings.h"

// Use None when metadata is disabled or the stored setting is unknown.
inline library::GroupKind configuredLibraryGroupKind() {
  if (SETTINGS.libraryUseMetadata == 0) return library::GroupKind::None;
  const uint8_t kind = SETTINGS.libraryGroupBy;
  return library::isKnownGroupKind(kind) ? static_cast<library::GroupKind>(kind) : library::GroupKind::None;
}
