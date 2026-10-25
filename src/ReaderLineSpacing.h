#pragma once

#include <I18n.h>

#include <iterator>

#include "CrossPointSettings.h"

// Labels and multipliers follow the persisted LINE_COMPRESSION order.
inline constexpr StrId LINE_SPACING_IDS[] = {StrId::STR_SPACING_1_0, StrId::STR_SPACING_1_1,  StrId::STR_SPACING_1_2,
                                             StrId::STR_SPACING_1_3, StrId::STR_SPACING_1_45, StrId::STR_SPACING_1_6,
                                             StrId::STR_SPACING_1_75};
inline constexpr float LINE_SPACING_MULTIPLIERS[] = {1.0f, 1.1f, 1.2f, 1.3f, 1.45f, 1.6f, 1.75f};
static_assert(std::size(LINE_SPACING_IDS) == CrossPointSettings::LINE_COMPRESSION_COUNT);
static_assert(std::size(LINE_SPACING_MULTIPLIERS) == CrossPointSettings::LINE_COMPRESSION_COUNT);
