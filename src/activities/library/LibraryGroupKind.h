#pragma once

#include <LibraryFormat.h>

#include "CrossPointSettings.h"

// Settings and persisted GroupKind values must match.
static_assert(CrossPointSettings::GROUP_BY_NONE == static_cast<uint8_t>(library::GroupKind::None));
static_assert(CrossPointSettings::GROUP_BY_SERIES == static_cast<uint8_t>(library::GroupKind::Series));
static_assert(CrossPointSettings::GROUP_BY_PUBLISHER == static_cast<uint8_t>(library::GroupKind::Publisher));
static_assert(CrossPointSettings::GROUP_BY_LANGUAGE == static_cast<uint8_t>(library::GroupKind::Language));
static_assert(CrossPointSettings::GROUP_BY_SUBJECT == static_cast<uint8_t>(library::GroupKind::Subject));
static_assert(CrossPointSettings::GROUP_BY_COUNT == library::GROUP_KIND_COUNT);

// Use None when metadata is disabled or the stored setting is unknown.
inline library::GroupKind configuredLibraryGroupKind() {
  if (SETTINGS.libraryUseMetadata == 0) return library::GroupKind::None;
  const uint8_t kind = SETTINGS.libraryGroupBy;
  return library::isKnownGroupKind(kind) ? static_cast<library::GroupKind>(kind) : library::GroupKind::None;
}
