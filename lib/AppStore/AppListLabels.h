#pragma once

#include "AppCatalogEntry.h"
#include "AppRegistryJson.h"

#include <string>

struct AppRowLabels {
  std::string subtitle;
  std::string rowValue;
};

namespace AppListLabels {

AppRowLabels buildInstalledRowLabels(const AppRegistryEntry& installed, const AppCatalogEntry* catalogOrNull,
                                     uint8_t utcOffsetQuarterHoursBiased, bool use12Hour);

AppRowLabels buildDiscoverRowLabels(const AppCatalogEntry& catalog, const AppRegistryEntry* installedOrNull,
                                    uint8_t utcOffsetQuarterHoursBiased, bool use12Hour);

}  // namespace AppListLabels
