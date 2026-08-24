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
