#include <gtest/gtest.h>

#include "lib/AppStore/AppListLabels.h"

TEST(AppListLabelsTest, DiscoverNotInstalledShowsCatalogVersionOnly) {
  AppCatalogEntry catalog;
  catalog.id = "hello";
  catalog.name = "Hello";
  catalog.version = "1.0.0";

  const auto labels = AppListLabels::buildDiscoverRowLabels(catalog, nullptr, 48, false);
  EXPECT_TRUE(labels.subtitle.empty());
  EXPECT_EQ(labels.rowValue, "1.0.0");
}

TEST(AppListLabelsTest, DiscoverInstalledSameVersion) {
  AppCatalogEntry catalog;
  catalog.version = "1.0.0";

  AppRegistryEntry installed;
  installed.version = "1.0.0";
  installed.installedAt = "2026-07-03T12:00:00Z";

  const auto labels = AppListLabels::buildDiscoverRowLabels(catalog, &installed, 48, false);
  EXPECT_EQ(labels.subtitle, "Jul 3, 2026 12:00");
  EXPECT_EQ(labels.rowValue, "1.0.0");
}

TEST(AppListLabelsTest, DiscoverInstalledUpdateAvailable) {
  AppCatalogEntry catalog;
  catalog.version = "1.1.0";

  AppRegistryEntry installed;
  installed.version = "1.0.0";
  installed.installedAt = "2026-07-03T12:00:00Z";

  const auto labels = AppListLabels::buildDiscoverRowLabels(catalog, &installed, 48, false);
  EXPECT_EQ(labels.subtitle, "Jul 3, 2026 12:00");
  EXPECT_EQ(labels.rowValue, "1.0.0 → 1.1.0");
}

TEST(AppListLabelsTest, InstalledRowWithoutCatalogEntry) {
  AppRegistryEntry installed;
  installed.version = "2.0.0";
  installed.installedAt = "2026-06-28T08:15:00Z";

  const auto labels = AppListLabels::buildInstalledRowLabels(installed, nullptr, 48, false);
  EXPECT_EQ(labels.subtitle, "Jun 28, 2026 08:15");
  EXPECT_EQ(labels.rowValue, "2.0.0");
}

TEST(AppListLabelsTest, InstalledRowWithUpdateAvailable) {
  AppCatalogEntry catalog;
  catalog.version = "2.1.0";

  AppRegistryEntry installed;
  installed.version = "2.0.0";
  installed.installedAt = "2026-06-28T08:15:00Z";

  const auto labels = AppListLabels::buildInstalledRowLabels(installed, &catalog, 48, false);
  EXPECT_EQ(labels.rowValue, "2.0.0 → 2.1.0");
}
