#include "AppListLabels.h"

#include "AppDateTimeFormat.h"
#include "AppVersion.h"

namespace {

std::string formatVersionArrow(const std::string& installedVersion, const std::string& catalogVersion) {
  const auto installed = AppVersion::parse(installedVersion);
  const auto catalog = AppVersion::parse(catalogVersion);
  if (installed.has_value() && catalog.has_value() && catalog->isNewerThan(*installed)) {
    return installedVersion + " → " + catalogVersion;
  }
  return installedVersion;
}

std::string formatInstallDateSubtitle(const AppRegistryEntry& installed, const uint8_t utcOffsetQuarterHoursBiased,
                                      const bool use12Hour) {
  return AppDateTimeFormat::formatIso8601ForDisplay(installed.installedAt, utcOffsetQuarterHoursBiased, use12Hour);
}

}  // namespace

namespace AppListLabels {

AppRowLabels buildInstalledRowLabels(const AppRegistryEntry& installed, const AppCatalogEntry* catalogOrNull,
                                     const uint8_t utcOffsetQuarterHoursBiased, const bool use12Hour) {
  AppRowLabels labels;
  labels.subtitle = formatInstallDateSubtitle(installed, utcOffsetQuarterHoursBiased, use12Hour);

  if (catalogOrNull != nullptr) {
    labels.rowValue = formatVersionArrow(installed.version, catalogOrNull->version);
  } else {
    labels.rowValue = installed.version;
  }

  return labels;
}

AppRowLabels buildDiscoverRowLabels(const AppCatalogEntry& catalog, const AppRegistryEntry* installedOrNull,
                                    const uint8_t utcOffsetQuarterHoursBiased, const bool use12Hour) {
  AppRowLabels labels;

  if (installedOrNull != nullptr) {
    labels.subtitle = formatInstallDateSubtitle(*installedOrNull, utcOffsetQuarterHoursBiased, use12Hour);
    labels.rowValue = formatVersionArrow(installedOrNull->version, catalog.version);
    return labels;
  }

  labels.rowValue = catalog.version;
  return labels;
}

}  // namespace AppListLabels
