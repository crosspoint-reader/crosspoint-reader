#include <gtest/gtest.h>

#include "util/PluginLocations.h"

TEST(PluginManifestClassification, InstallingMarkerQuarantinesOtherwiseValidPlugins) {
  EXPECT_TRUE(PluginLocations::isDiscoverableEntry(false, false, true, false));
  EXPECT_TRUE(PluginLocations::isDiscoverableEntry(false, true, false, false));
  EXPECT_TRUE(PluginLocations::isDiscoverableEntry(false, false, false, true));
  EXPECT_FALSE(PluginLocations::isDiscoverableEntry(true, true, true, true));
  EXPECT_FALSE(PluginLocations::isDiscoverableEntry(false, false, false, false));
}

TEST(PluginManifestClassification, DirectPluginBundlesMayReuseInstalledRoot) {
  EXPECT_TRUE(PluginLocations::shouldReuseInstalledBundleDir("/.crosspoint/plugins", "foo"));
  EXPECT_TRUE(PluginLocations::shouldReuseInstalledBundleDir("/plugins", "foo"));
  EXPECT_TRUE(PluginLocations::shouldReuseInstalledBundleDir("/.plugins", "foo"));

  EXPECT_FALSE(PluginLocations::shouldReuseInstalledBundleDir("/downloads", "foo"));
  EXPECT_FALSE(PluginLocations::shouldReuseInstalledBundleDir("/.crosspoint/plugins", "foo/assets"));
  EXPECT_FALSE(PluginLocations::shouldReuseInstalledBundleDir("/.crosspoint/plugins", ""));
}

TEST(PluginManifestClassification, BundleEntryUrlPathEncodesWhatARequestLineCannotCarry) {
  // Plain and nested ASCII entries are unchanged.
  EXPECT_EQ(PluginLocations::bundleEntryUrlPath("plugin.js"), "plugin.js");
  EXPECT_EQ(PluginLocations::bundleEntryUrlPath("assets/icon-1_a.bin"), "assets/icon-1_a.bin");
  // Spaces and UTF-8 bytes are percent-encoded; the SD name stays as listed.
  EXPECT_EQ(PluginLocations::bundleEntryUrlPath("H\xC3\xB4m nay.txt"), "H%C3%B4m%20nay.txt");
  // '?' and '#' would end the path; already-encoded names are not encoded twice.
  EXPECT_EQ(PluginLocations::bundleEntryUrlPath("a?b#c.txt"), "a%3Fb%23c.txt");
  EXPECT_EQ(PluginLocations::bundleEntryUrlPath("a%20b.txt"), "a%20b.txt");
}

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

TEST(PluginManifestClassification, MenuWinsOverCatalogAndEvents) {
  using PluginLocations::DeviceKind;
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(false, false, true), DeviceKind::Menu);
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(true, false, true), DeviceKind::Menu);
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(true, true, true), DeviceKind::Menu);
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(false, true, true), DeviceKind::Menu);
}

TEST(PluginManifestClassification, MenuOpensEvenWithReadme) {
  EXPECT_EQ(PluginLocations::pickerAction(PluginLocations::DeviceKind::Menu, true),
            PluginLocations::PickerAction::Menu);
  EXPECT_EQ(PluginLocations::pickerAction(PluginLocations::DeviceKind::Menu, false),
            PluginLocations::PickerAction::Menu);
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

TEST(PluginManifestClassification, DocumentReturnsOnlyToItsOwnPlugin) {
  const std::string manifest = "/plugins/WADN/device.json";
  EXPECT_TRUE(PluginLocations::isInManifestDir(manifest, "/plugins/WADN/today.txt"));
  EXPECT_TRUE(PluginLocations::isInManifestDir(manifest, "/plugins/WADN/notes/week.txt"));
  EXPECT_FALSE(PluginLocations::isInManifestDir(manifest, "/plugins/WADNX/today.txt"));
  EXPECT_FALSE(PluginLocations::isInManifestDir(manifest, "/plugins/WADN/"));
  EXPECT_FALSE(PluginLocations::isInManifestDir(manifest, "/books/today.txt"));
  EXPECT_FALSE(PluginLocations::isInManifestDir("", "/plugins/WADN/today.txt"));
  EXPECT_FALSE(PluginLocations::isInManifestDir("device.json", "/today.txt"));
}
