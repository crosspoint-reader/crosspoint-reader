#include <gtest/gtest.h>

#include "util/PluginLocations.h"

TEST(PluginManifestClassification, BrowseUrlIsCatalog) {
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(true, false), PluginLocations::DeviceKind::Catalog);
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(true, true), PluginLocations::DeviceKind::Catalog);
}

TEST(PluginManifestClassification, EventsWithoutBrowseAreBackgroundOnly) {
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(false, true), PluginLocations::DeviceKind::Background);
}

TEST(PluginManifestClassification, EmptyDeviceManifestIsNotOpenable) {
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(false, false), PluginLocations::DeviceKind::None);
}

TEST(PluginManifestClassification, BackgroundManifestWithReadmeOpensInfo) {
  EXPECT_EQ(PluginLocations::pickerAction(PluginLocations::DeviceKind::Background, true),
            PluginLocations::PickerAction::Readme);
}

TEST(PluginManifestClassification, BackgroundManifestWithoutReadmeStaysInPicker) {
  EXPECT_EQ(PluginLocations::pickerAction(PluginLocations::DeviceKind::Background, false),
            PluginLocations::PickerAction::None);
}

TEST(PluginManifestClassification, WebOnlyPluginWithReadmeOpensInfo) {
  EXPECT_EQ(PluginLocations::pickerAction(PluginLocations::DeviceKind::None, true),
            PluginLocations::PickerAction::Readme);
  EXPECT_EQ(PluginLocations::pickerAction(PluginLocations::DeviceKind::None, false),
            PluginLocations::PickerAction::None);
}

TEST(PluginManifestClassification, CatalogOpensEvenWithReadme) {
  EXPECT_EQ(PluginLocations::pickerAction(PluginLocations::DeviceKind::Catalog, true),
            PluginLocations::PickerAction::Catalog);
}

TEST(PluginManifestClassification, OnlyANewerCatalogVersionIsAnUpdate) {
  EXPECT_TRUE(PluginLocations::isNewerVersion("1.1.0", "1.0.0"));
  EXPECT_TRUE(PluginLocations::isNewerVersion("0.1.10", "0.1.9"));
  EXPECT_TRUE(PluginLocations::isNewerVersion("2.0.0", "1.9.9"));
  EXPECT_FALSE(PluginLocations::isNewerVersion("1.0.0", "1.1.0"));  // no downgrade offers
  EXPECT_FALSE(PluginLocations::isNewerVersion("1.2.0", "1.2.0"));
  EXPECT_TRUE(PluginLocations::isNewerVersion("1.0.0", ""));  // installed copy without a version
  EXPECT_FALSE(PluginLocations::isNewerVersion("", "1.0.0"));
  EXPECT_FALSE(PluginLocations::isNewerVersion("1.2", "1.0.0"));  // catalog must be MAJOR.MINOR.PATCH
}

TEST(PluginManifestClassification, VersionsMustBeExactlyThreeDigitComponents) {
  for (const char* bad :
       {"1.2.3-beta", "1.2.3 ", "1.2.3.4", "-1.2.3", "+1.2.3", " 1.2.3", "1..3", "1.2.", "1.2.99999999999", "v1.2.3"}) {
    EXPECT_FALSE(PluginLocations::isNewerVersion(bad, "0.0.1")) << bad;  // never offered
    EXPECT_TRUE(PluginLocations::isNewerVersion("0.0.1", bad)) << bad;   // installed treated as unversioned
  }
  EXPECT_TRUE(PluginLocations::isNewerVersion("4294967295.0.0", "1.0.0"));  // UINT32_MAX still fits
}
