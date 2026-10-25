#pragma once

#include <I18n.h>

#include <iterator>

#include "CrossPointSettings.h"

// Labels and multipliers follow the persisted LINE_COMPRESSION order.
inline constexpr StrId LINE_SPACING_IDS[] = {StrId::STR_SPACING_110_PERCENT, StrId::STR_SPACING_120_PERCENT,
                                             StrId::STR_SPACING_130_PERCENT, StrId::STR_SPACING_145_PERCENT,
                                             StrId::STR_SPACING_160_PERCENT, StrId::STR_SPACING_175_PERCENT};
inline constexpr float LINE_SPACING_MULTIPLIERS[] = {1.1f, 1.2f, 1.3f, 1.45f, 1.6f, 1.75f};
static_assert(std::size(LINE_SPACING_IDS) == CrossPointSettings::LINE_COMPRESSION_COUNT);
static_assert(std::size(LINE_SPACING_MULTIPLIERS) == CrossPointSettings::LINE_COMPRESSION_COUNT);
