#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Mock headers must come before any src/ includes that pull in hardware headers.
#include "include/FeatureFlags.h"
#include "lib/FsHelpers/FsHelpers.h"
#include "lib/Markdown/MarkdownParser.h"
#include "lib/Serialization/Serialization.h"
#include "src/core/features/FeatureCatalog.h"
#include "src/fontIds.h"
#include "src/util/ForkDriftNavigation.h"
#include "test/mock/Arduino.h"
#include "test/mock/HalStorage.h"
#include "test/mock/SpiBusMutex.h"
// Keep this undef as a defensive guard for host builds that include pthread/time headers.
#undef TIME_UTC
#include "src/CrossPointSettings.h"
#include "src/activities/todo/TodoPlannerStorage.h"
#include "src/util/InputValidation.h"
#include "src/util/PathUtils.h"
#include "src/util/UsbMscPrompt.h"

// Mock ESP implementation
MockESP ESP;

// ── Existing tests ────────────────────────────────────────────────────────

void testPathNormalisation() {
  std::cout << "Testing Path Normalisation..." << std::endl;
  assert(FsHelpers::normalisePath("/a/b/c") == "a/b/c");
  assert(FsHelpers::normalisePath("a/b/../c") == "a/c");
  assert(FsHelpers::normalisePath("a/b/../../c") == "c");
  assert(FsHelpers::normalisePath("///a//b/") == "a/b");
  assert(FsHelpers::normalisePath("test/../test/dir") == "test/dir");
  assert(FsHelpers::normalisePath("a/b/.") == "a/b");
  assert(FsHelpers::normalisePath("a/b/..") == "a");
  assert(FsHelpers::normalisePath("a/./b/../c/.") == "a/c");
  std::cout << "Path Normalisation tests passed!" << std::endl;
}

void testMarkdownLimits() {
  std::cout << "Testing Markdown Parser Limits..." << std::endl;
  MarkdownParser parser;

  std::cout << "  Testing input size limit..." << std::endl;
  std::string largeInput(MarkdownParser::MAX_INPUT_SIZE + 1, 'a');
  auto resultLarge = parser.parse(largeInput);
  assert(resultLarge == nullptr);

  std::cout << "  Testing nesting depth limit..." << std::endl;
  std::string deepNesting = "";
  for (int i = 0; i < 60; ++i) {
    deepNesting += "> ";
  }
  deepNesting += "Deep";
  auto resultDeep = parser.parse(deepNesting);
  assert(resultDeep == nullptr);

  std::cout << "  Testing node count limit..." << std::endl;
  std::string manyNodes = "";
  for (int i = 0; i < 6000; ++i) {
    manyNodes += "p\n\n";
  }
  auto resultMany = parser.parse(manyNodes);
  assert(resultMany == nullptr);

  std::cout << "Markdown Parser Limits tests passed!" << std::endl;
}

void testTodoPlannerStorageSelection() {
  std::cout << "Testing TODO Planner storage selection..." << std::endl;
  const std::string isoDate = "2026-02-17";
  const std::string alternateDate = "17.02.2026";
  assert(TodoPlannerStorage::dailyPath(isoDate, true, true, false) == "/daily/2026-02-17.md");
  assert(TodoPlannerStorage::dailyPath(isoDate, false, false, true) == "/daily/2026-02-17.txt");
  assert(TodoPlannerStorage::dailyPath(isoDate, false, true, false) == "/daily/2026-02-17.md");
  assert(TodoPlannerStorage::dailyPath(isoDate, true, false, false) == "/daily/2026-02-17.md");
  assert(TodoPlannerStorage::dailyPath(isoDate, false, false, false) == "/daily/2026-02-17.txt");
  assert(TodoPlannerStorage::dailyPath(alternateDate, false, false, false) == "/daily/17.02.2026.txt");
  assert(TodoPlannerStorage::formatEntry("Task", false) == "- [ ] Task");
  assert(TodoPlannerStorage::formatEntry("Agenda item", true) == "Agenda item");
  std::cout << "TODO Planner storage selection tests passed!" << std::endl;
}

void testInputValidation() {
  std::cout << "Testing input validation hardening..." << std::endl;

  size_t index = 0;
  assert(!InputValidation::findAsciiControlChar("/ok/path", 8, index));

  const std::string hasNewline = "/bad\npath";
  assert(InputValidation::findAsciiControlChar(hasNewline.c_str(), hasNewline.size(), index));
  assert(index == 4);

  const std::string hasDel = std::string("abc") + static_cast<char>(0x7F) + "def";
  assert(InputValidation::findAsciiControlChar(hasDel.c_str(), hasDel.size(), index));
  assert(index == 3);

  const std::string hasNull = std::string("ab\0cd", 5);
  assert(InputValidation::findAsciiControlChar(hasNull.c_str(), hasNull.size(), index));
  assert(index == 2);

  size_t parsed = 0;
  assert(InputValidation::parseStrictPositiveSize("1", 1, 100, parsed) && parsed == 1);
  assert(InputValidation::parseStrictPositiveSize("512", 3, 512, parsed) && parsed == 512);
  assert(InputValidation::parseStrictPositiveSize("0010", 4, 100, parsed) && parsed == 10);
  assert(InputValidation::parseStrictPositiveSize("536870912", 9, 536870912ULL, parsed) && parsed == 536870912ULL);

  assert(!InputValidation::parseStrictPositiveSize("", 0, 100, parsed));
  assert(!InputValidation::parseStrictPositiveSize("0", 1, 100, parsed));
  assert(!InputValidation::parseStrictPositiveSize("101", 3, 100, parsed));
  assert(!InputValidation::parseStrictPositiveSize("536870913", 9, 536870912ULL, parsed));
  assert(!InputValidation::parseStrictPositiveSize("12a", 3, 100, parsed));
  assert(!InputValidation::parseStrictPositiveSize(nullptr, 0, 100, parsed));

  const std::string huge = "184467440737095516161844674407370955161";
  assert(!InputValidation::parseStrictPositiveSize(huge.c_str(), huge.size(), static_cast<size_t>(-1), parsed));

  std::cout << "Input validation hardening tests passed!" << std::endl;
}

void testFeatureCatalogApi() {
  std::cout << "Testing core feature catalog API..." << std::endl;

  size_t featureCount = 0;
  const core::FeatureDescriptor* features = core::FeatureCatalog::all(featureCount);
  assert(features != nullptr);
  assert(featureCount > 0);
  assert(core::FeatureCatalog::totalCount() == featureCount);

  assert(core::FeatureCatalog::isEnabled("epub_support") == (ENABLE_EPUB_SUPPORT != 0));
  assert(core::FeatureCatalog::isEnabled("home_media_picker") == (ENABLE_HOME_MEDIA_PICKER != 0));
  assert(core::FeatureCatalog::isEnabled("missing_feature") == false);
  assert(core::FeatureCatalog::find("missing_feature") == nullptr);

  const String json = core::FeatureCatalog::toJson();
  assert(!json.isEmpty());
  assert(json.indexOf("\"epub_support\":") != -1);
  assert(json.indexOf("\"todo_planner\":") != -1);

  const String buildString = core::FeatureCatalog::buildString();
  assert(!buildString.isEmpty());

  String dependencyError;
  assert(core::FeatureCatalog::validate(&dependencyError));
  assert(dependencyError.isEmpty());

  std::cout << "Core feature catalog API tests passed!" << std::endl;
}

// ── New tests ─────────────────────────────────────────────────────────────

// Persisted fields (in serialization order) as of SETTINGS_COUNT=39:
//  1  sleepScreen
//  2  extraParagraphSpacing
//  3  shortPwrBtn
//  4  statusBar
//  5  orientation
//  6  frontButtonLayout (legacy — used by applyLegacyFrontButtonLayout on load)
//  7  sideButtonLayout
//  8  fontFamily
//  9  fontSize
// 10  lineSpacing
// 11  paragraphAlignment
// 12  sleepTimeout
// 13  refreshFrequency
// 14  screenMargin
// 15  sleepScreenCoverMode
// 16  opdsServerUrl (string)
// 17  textAntiAliasing
// 18  hideBatteryPercentage
// 19  longPressChapterSkip
// 20  hyphenationEnabled
// 21  opdsUsername (string)
// 22  opdsPassword (string)
// 23  sleepScreenCoverFilter
// 24  backgroundServerOnCharge
// 25  todoFallbackCover
// 26  timeMode
// 27  timeZoneOffset
// 28  lastTimeSyncEpoch
// 29  releaseChannel
// 30  sleepScreenSource
// 31  userFontPath (string)
// 32  usbMscPromptOnConnect
// 33  selectedOtaBundle (string)
// 34  installedOtaBundle (string)
// 35  installedOtaFeatureFlags (string)
// 36  frontButtonBack   (remap)
// 37  frontButtonConfirm (remap)
// 38  frontButtonLeft   (remap)
// 39  frontButtonRight  (remap)
//
// NOTE: The JSON format saves and restores the explicit per-button remap fields
// directly. The test pre-applies the LEFT_RIGHT_BACK_CONFIRM preset values before
// saving so the assertions verify the correct round-trip of a custom mapping.
//
// NOT persisted: fadingFix, embeddedStyle, darkMode (runtime-only fields).

void testSettingsRoundTrip() {
  std::cout << "Testing CrossPointSettings round-trip serialization..." << std::endl;

  // Reset in-memory filesystem between tests.
  Storage.reset();

  CrossPointSettings& s = CrossPointSettings::getInstance();

  // Set every persisted field to a distinctive non-default canary value.
  s.sleepScreen = CrossPointSettings::LIGHT;
  s.sleepScreenCoverMode = CrossPointSettings::CROP;
  s.sleepScreenCoverFilter = CrossPointSettings::INVERTED_BLACK_AND_WHITE;
  s.sleepScreenSource = CrossPointSettings::SLEEP_SOURCE_POKEDEX;
  s.statusBar = CrossPointSettings::NO_PROGRESS;
  s.extraParagraphSpacing = 0;
  s.textAntiAliasing = 0;
  s.shortPwrBtn = CrossPointSettings::SLEEP;
  s.orientation = CrossPointSettings::LANDSCAPE_CW;
  // Use LEFT_RIGHT_BACK_CONFIRM as the layout preset and apply the matching remap.
  // The JSON format saves and restores the explicit per-button mapping directly.
  s.frontButtonLayout = CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM;
  s.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
  s.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
  s.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
  s.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
  s.sideButtonLayout = CrossPointSettings::NEXT_PREV;
  s.fontFamily = CrossPointSettings::NOTOSANS;
  s.fontSize = CrossPointSettings::LARGE;
  s.lineSpacing = CrossPointSettings::WIDE;
  s.paragraphAlignment = CrossPointSettings::CENTER_ALIGN;
  s.sleepTimeout = CrossPointSettings::SLEEP_30_MIN;
  s.refreshFrequency = CrossPointSettings::REFRESH_10;
  s.hyphenationEnabled = 1;
  s.screenMargin = 12;
  strncpy(s.opdsServerUrl, "http://calibre.local:8080", sizeof(s.opdsServerUrl) - 1);
  strncpy(s.opdsUsername, "testuser", sizeof(s.opdsUsername) - 1);
  strncpy(s.opdsPassword, "s3cr3t!", sizeof(s.opdsPassword) - 1);
  s.hideBatteryPercentage = CrossPointSettings::HIDE_READER;
  s.uiTheme = CrossPointSettings::LYRA;
  s.longPressChapterSkip = 0;
  s.backgroundServerOnCharge = 1;
  s.todoFallbackCover = 1;
  s.timeMode = CrossPointSettings::TIME_MODE_LOCAL;
  s.timeZoneOffset = 14;
  s.lastTimeSyncEpoch = 1700000000UL;
  s.releaseChannel = CrossPointSettings::RELEASE_NIGHTLY;
  s.usbMscPromptOnConnect = 1;
  strncpy(s.userFontPath, "/fonts/MyFont.ttf", sizeof(s.userFontPath) - 1);
  strncpy(s.selectedOtaBundle, "bundle-abc123", sizeof(s.selectedOtaBundle) - 1);
  strncpy(s.installedOtaBundle, "bundle-xyz789", sizeof(s.installedOtaBundle) - 1);
  strncpy(s.installedOtaFeatureFlags, "epub_support,ota_updates", sizeof(s.installedOtaFeatureFlags) - 1);

  // ── Save ──────────────────────────────────────────────────────────────
  assert(s.saveToFile());

  // ── Reset persisted fields to defaults, then reload ──────────────────
  s.sleepScreen = CrossPointSettings::DARK;
  s.sleepScreenCoverMode = CrossPointSettings::FIT;
  s.sleepScreenCoverFilter = CrossPointSettings::NO_FILTER;
  s.sleepScreenSource = CrossPointSettings::SLEEP_SOURCE_SLEEP;
  s.statusBar = CrossPointSettings::FULL;
  s.extraParagraphSpacing = 1;
  s.textAntiAliasing = 1;
  s.shortPwrBtn = CrossPointSettings::IGNORE;
  s.orientation = CrossPointSettings::PORTRAIT;
  s.frontButtonLayout = CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT;
  s.sideButtonLayout = CrossPointSettings::PREV_NEXT;
  s.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
  s.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
  s.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
  s.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
  s.fontFamily = CrossPointSettings::BOOKERLY;
  s.fontSize = CrossPointSettings::MEDIUM;
  s.lineSpacing = CrossPointSettings::NORMAL;
  s.paragraphAlignment = CrossPointSettings::JUSTIFIED;
  s.sleepTimeout = CrossPointSettings::SLEEP_10_MIN;
  s.refreshFrequency = CrossPointSettings::REFRESH_15;
  s.hyphenationEnabled = 0;
  s.screenMargin = 5;
  s.opdsServerUrl[0] = '\0';
  s.opdsUsername[0] = '\0';
  s.opdsPassword[0] = '\0';
  s.hideBatteryPercentage = CrossPointSettings::HIDE_NEVER;
  s.uiTheme = CrossPointSettings::LYRA;
  s.longPressChapterSkip = 1;
  s.backgroundServerOnCharge = 0;
  s.todoFallbackCover = 0;
  s.timeMode = CrossPointSettings::TIME_MODE_UTC;
  s.timeZoneOffset = 12;
  s.lastTimeSyncEpoch = 0;
  s.releaseChannel = CrossPointSettings::RELEASE_STABLE;
  s.usbMscPromptOnConnect = 0;
  s.userFontPath[0] = '\0';
  s.selectedOtaBundle[0] = '\0';
  s.installedOtaBundle[0] = '\0';
  s.installedOtaFeatureFlags[0] = '\0';

  assert(s.loadFromFile());

  // ── Verify every persisted canary value was round-tripped ─────────────
  assert(s.sleepScreen == CrossPointSettings::LIGHT);
  assert(s.sleepScreenCoverMode == CrossPointSettings::CROP);
  assert(s.sleepScreenCoverFilter == CrossPointSettings::INVERTED_BLACK_AND_WHITE);
  assert(s.sleepScreenSource == CrossPointSettings::SLEEP_SOURCE_POKEDEX);
  assert(s.statusBar == CrossPointSettings::NO_PROGRESS);
  assert(s.extraParagraphSpacing == 0);
  assert(s.textAntiAliasing == 0);
  assert(s.shortPwrBtn == CrossPointSettings::SLEEP);
  assert(s.orientation == CrossPointSettings::LANDSCAPE_CW);
  assert(s.fontFamily == CrossPointSettings::NOTOSANS);
  assert(s.fontSize == CrossPointSettings::LARGE);
  assert(s.lineSpacing == CrossPointSettings::WIDE);
  assert(s.paragraphAlignment == CrossPointSettings::CENTER_ALIGN);
  assert(s.sleepTimeout == CrossPointSettings::SLEEP_30_MIN);
  assert(s.refreshFrequency == CrossPointSettings::REFRESH_10);
  assert(s.hyphenationEnabled == 1);
  assert(s.screenMargin == 12);
  assert(std::string(s.opdsServerUrl) == "http://calibre.local:8080");
  assert(std::string(s.opdsUsername) == "testuser");
  assert(std::string(s.opdsPassword) == "s3cr3t!");
  assert(s.hideBatteryPercentage == CrossPointSettings::HIDE_READER);
  assert(s.longPressChapterSkip == 0);
  assert(s.backgroundServerOnCharge == 1);
  assert(s.todoFallbackCover == 1);
  assert(s.timeMode == CrossPointSettings::TIME_MODE_LOCAL);
  assert(s.timeZoneOffset == 14);
  assert(s.lastTimeSyncEpoch == 1700000000UL);
  assert(s.releaseChannel == CrossPointSettings::RELEASE_NIGHTLY);
  assert(s.usbMscPromptOnConnect == 1);
  assert(std::string(s.userFontPath) == "/fonts/MyFont.ttf");
  assert(std::string(s.selectedOtaBundle) == "bundle-abc123");
  assert(std::string(s.installedOtaBundle) == "bundle-xyz789");
  assert(std::string(s.installedOtaFeatureFlags) == "epub_support,ota_updates");
  // The frontButtonLayout field is persisted and the legacy preset is applied on load.
  // For LEFT_RIGHT_BACK_CONFIRM the preset produces:
  //   back=FRONT_HW_LEFT(2), confirm=FRONT_HW_RIGHT(3),
  //   left=FRONT_HW_BACK(0),  right=FRONT_HW_CONFIRM(1)
  assert(s.frontButtonLayout == CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM);
  assert(s.frontButtonBack == CrossPointSettings::FRONT_HW_LEFT);
  assert(s.frontButtonConfirm == CrossPointSettings::FRONT_HW_RIGHT);
  assert(s.frontButtonLeft == CrossPointSettings::FRONT_HW_BACK);
  assert(s.frontButtonRight == CrossPointSettings::FRONT_HW_CONFIRM);

  std::cout << "CrossPointSettings round-trip tests passed!" << std::endl;
}

void testSettingsTruncatedLoad() {
  std::cout << "Testing CrossPointSettings graceful load from older file..." << std::endl;

  Storage.reset();
  CrossPointSettings& s = CrossPointSettings::getInstance();

  // Write a partial file with only a few fields (simulates v1/v2 firmware file).
  // Serialization order: sleepScreen(1), extraParagraphSpacing(2), shortPwrBtn(3), ...
  {
    FsFile file;
    Storage.openFileForWrite("TEST", "/.crosspoint/settings.bin", file);

    const uint8_t version = 4;
    const uint8_t count = 3;  // only 3 fields present
    serialization::writePod(file, version);
    serialization::writePod(file, count);
    const uint8_t sleepVal = CrossPointSettings::LIGHT;
    const uint8_t spacingVal = 0;
    const uint8_t pwrVal = CrossPointSettings::SLEEP;
    serialization::writePod(file, sleepVal);
    serialization::writePod(file, spacingVal);
    serialization::writePod(file, pwrVal);
    file.close();
  }

  // Set fields to non-default values, then load the partial file.
  // Only the 3 written fields should change; the rest stay at their pre-load values.
  s.sleepScreen = CrossPointSettings::DARK;
  s.extraParagraphSpacing = 1;
  s.shortPwrBtn = CrossPointSettings::IGNORE;
  s.fontFamily = CrossPointSettings::BOOKERLY;

  assert(s.loadFromFile());

  // Fields 1-3 are in the file and should be updated.
  assert(s.sleepScreen == CrossPointSettings::LIGHT);
  assert(s.extraParagraphSpacing == 0);
  assert(s.shortPwrBtn == CrossPointSettings::SLEEP);
  // Field 8 (fontFamily) was not in the file, so it is unchanged.
  assert(s.fontFamily == CrossPointSettings::BOOKERLY);

  std::cout << "CrossPointSettings truncated-load tests passed!" << std::endl;
}

void testPathUtilsSecurity() {
  std::cout << "Testing PathUtils security functions..." << std::endl;

  // ── containsTraversal ────────────────────────────────────────────────
  assert(PathUtils::containsTraversal("/../secret"));
  assert(PathUtils::containsTraversal("/books/.."));
  assert(PathUtils::containsTraversal("../etc/passwd"));
  assert(PathUtils::containsTraversal(".."));
  assert(PathUtils::containsTraversal("%2e%2e%2f"));
  assert(PathUtils::containsTraversal("/foo/%2f%2e%2e"));
  assert(PathUtils::containsTraversal("/foo/..%2fbar"));
  assert(PathUtils::containsTraversal("/foo/%2f../bar"));
  assert(!PathUtils::containsTraversal("/books/my..chapter"));
  assert(!PathUtils::containsTraversal("/valid/path"));

  // ── isValidSdPath ────────────────────────────────────────────────────
  assert(PathUtils::isValidSdPath("/books/novel.epub"));
  assert(PathUtils::isValidSdPath("/"));
  assert(!PathUtils::isValidSdPath(""));
  assert(!PathUtils::isValidSdPath("/../secret"));
  {
    String longPath("/");
    for (int i = 0; i < 260; ++i) longPath += 'a';
    assert(!PathUtils::isValidSdPath(longPath));
  }
  {
    String ctrlPath("/bad");
    ctrlPath += '\n';
    assert(!PathUtils::isValidSdPath(ctrlPath));
  }
  assert(!PathUtils::isValidSdPath("/bad\\path"));

  // ── normalizePath ────────────────────────────────────────────────────
  assert(PathUtils::normalizePath("") == "/");
  assert(PathUtils::normalizePath("books") == "/books");
  assert(PathUtils::normalizePath("/books/") == "/books");
  assert(PathUtils::normalizePath("//books//sub") == "/books/sub");
  assert(PathUtils::normalizePath("/") == "/");

  // ── urlDecode ────────────────────────────────────────────────────────
  assert(PathUtils::urlDecode("/hello%20world") == "/hello world");
  assert(PathUtils::urlDecode("/a+b") == "/a b");
  assert(PathUtils::urlDecode("/no%2Fslash") == "/no/slash");
  assert(PathUtils::urlDecode("/%") == "/%");  // invalid escape kept
  assert(PathUtils::urlDecode("/plain") == "/plain");

  // ── isValidFilename ──────────────────────────────────────────────────
  assert(PathUtils::isValidFilename("book.epub"));
  assert(PathUtils::isValidFilename("my novel.txt"));
  assert(!PathUtils::isValidFilename(""));
  assert(!PathUtils::isValidFilename("bad/name"));
  assert(!PathUtils::isValidFilename("bad\\name"));
  assert(!PathUtils::isValidFilename("bad:name"));
  assert(!PathUtils::isValidFilename("bad<name"));
  assert(!PathUtils::isValidFilename("bad>name"));
  assert(!PathUtils::isValidFilename("bad?name"));
  assert(!PathUtils::isValidFilename("bad|name"));
  assert(!PathUtils::isValidFilename("bad*name"));
  assert(!PathUtils::isValidFilename("bad\"name"));
  assert(!PathUtils::isValidFilename("."));
  assert(!PathUtils::isValidFilename(".."));
  {
    String longName;
    for (int i = 0; i < 260; ++i) longName += 'a';
    assert(!PathUtils::isValidFilename(longName));
  }

  std::cout << "PathUtils security tests passed!" << std::endl;
}

void testForkDriftCoverNavigation() {
  using ForkDriftNavigation::navigateCoverGrid;
  std::cout << "Testing ForkDrift cover grid navigation..." << std::endl;

  constexpr int cols = 3;
  constexpr int rows = 2;

  // ── Single-row: 1 book ────────────────────────────────────────────────
  {
    // Left/right wrap within the single book (stays at 0)
    auto r = navigateCoverGrid(0, 1, cols, rows, true, false, false, false);
    assert(r.bookIndex == 0 && !r.enterButtonGrid);
    r = navigateCoverGrid(0, 1, cols, rows, false, true, false, false);
    assert(r.bookIndex == 0 && !r.enterButtonGrid);
    // Up from row 0 → button grid
    r = navigateCoverGrid(0, 1, cols, rows, false, false, true, false);
    assert(r.enterButtonGrid);
    // Down → button grid (last row = row 0)
    r = navigateCoverGrid(0, 1, cols, rows, false, false, false, true);
    assert(r.enterButtonGrid);
  }

  // ── Single-row: 2 books ───────────────────────────────────────────────
  {
    // Right from 0 → 1
    auto r = navigateCoverGrid(0, 2, cols, rows, false, true, false, false);
    assert(r.bookIndex == 1 && !r.enterButtonGrid);
    // Right from 1 wraps to 0
    r = navigateCoverGrid(1, 2, cols, rows, false, true, false, false);
    assert(r.bookIndex == 0 && !r.enterButtonGrid);
    // Left from 0 wraps to 1
    r = navigateCoverGrid(0, 2, cols, rows, true, false, false, false);
    assert(r.bookIndex == 1 && !r.enterButtonGrid);
    // Left from 1 → 0
    r = navigateCoverGrid(1, 2, cols, rows, true, false, false, false);
    assert(r.bookIndex == 0 && !r.enterButtonGrid);
    // Up / Down → button grid
    r = navigateCoverGrid(0, 2, cols, rows, false, false, true, false);
    assert(r.enterButtonGrid);
    r = navigateCoverGrid(1, 2, cols, rows, false, false, false, true);
    assert(r.enterButtonGrid);
  }

  // ── Single-row: 3 books ───────────────────────────────────────────────
  {
    auto r = navigateCoverGrid(2, 3, cols, rows, false, true, false, false);
    assert(r.bookIndex == 0 && !r.enterButtonGrid);  // wraps to start
    r = navigateCoverGrid(0, 3, cols, rows, true, false, false, false);
    assert(r.bookIndex == 2 && !r.enterButtonGrid);  // wraps to end
    r = navigateCoverGrid(1, 3, cols, rows, false, false, false, true);
    assert(r.enterButtonGrid);  // row 0 is last book row
    r = navigateCoverGrid(1, 3, cols, rows, false, false, true, false);
    assert(r.enterButtonGrid);  // row 0, up → button grid
  }

  // ── Two rows: 4 books ─────────────────────────────────────────────────
  {
    // Down from row 0 (book 0) → row 1 (book 3, clamped)
    auto r = navigateCoverGrid(0, 4, cols, rows, false, false, false, true);
    assert(r.bookIndex == 3 && !r.enterButtonGrid);
    // Down from row 0 col 1 (book 1) → row 1 col 1 clamped to 3
    r = navigateCoverGrid(1, 4, cols, rows, false, false, false, true);
    assert(r.bookIndex == 3 && !r.enterButtonGrid);
    // Down from row 1 (book 3) → button grid
    r = navigateCoverGrid(3, 4, cols, rows, false, false, false, true);
    assert(r.enterButtonGrid);
    // Up from row 1 (book 3) → row 0 col 0 (book 0)
    r = navigateCoverGrid(3, 4, cols, rows, false, false, true, false);
    assert(r.bookIndex == 0 && !r.enterButtonGrid);
    // Up from row 0 → button grid
    r = navigateCoverGrid(2, 4, cols, rows, false, false, true, false);
    assert(r.enterButtonGrid);
    // Linear left/right wrap across rows
    r = navigateCoverGrid(3, 4, cols, rows, false, true, false, false);
    assert(r.bookIndex == 0 && !r.enterButtonGrid);  // 3+1 wraps to 0
    r = navigateCoverGrid(0, 4, cols, rows, true, false, false, false);
    assert(r.bookIndex == 3 && !r.enterButtonGrid);  // 0-1 wraps to 3
  }

  // ── Full grid: 6 books ────────────────────────────────────────────────
  {
    // Down from row 0 col 2 (book 2) → row 1 col 2 (book 5)
    auto r = navigateCoverGrid(2, 6, cols, rows, false, false, false, true);
    assert(r.bookIndex == 5 && !r.enterButtonGrid);
    // Down from row 1 → button grid
    r = navigateCoverGrid(4, 6, cols, rows, false, false, false, true);
    assert(r.enterButtonGrid);
    // Up from row 0 → button grid
    r = navigateCoverGrid(1, 6, cols, rows, false, false, true, false);
    assert(r.enterButtonGrid);
    // Up from row 1 → row 0
    r = navigateCoverGrid(5, 6, cols, rows, false, false, true, false);
    assert(r.bookIndex == 2 && !r.enterButtonGrid);
  }

  std::cout << "ForkDrift cover grid navigation tests passed!" << std::endl;
}

void testUsbMscPromptGate() {
  std::cout << "Testing USB MSC prompt host gating..." << std::endl;

  assert(!UsbMscPrompt::shouldShowOnUsbConnect(
      /*promptEnabled=*/true, /*usbConnected=*/true, /*usbConnectedLast=*/false, /*hostSupportsUsbSerial=*/false,
      /*sessionIdle=*/true));
  assert(!UsbMscPrompt::shouldShowOnUsbConnect(
      /*promptEnabled=*/true, /*usbConnected=*/false, /*usbConnectedLast=*/false, /*hostSupportsUsbSerial=*/true,
      /*sessionIdle=*/true));
  assert(!UsbMscPrompt::shouldShowOnUsbConnect(
      /*promptEnabled=*/true, /*usbConnected=*/true, /*usbConnectedLast=*/true, /*hostSupportsUsbSerial=*/true,
      /*sessionIdle=*/true));
  assert(!UsbMscPrompt::shouldShowOnUsbConnect(
      /*promptEnabled=*/false, /*usbConnected=*/true, /*usbConnectedLast=*/false, /*hostSupportsUsbSerial=*/true,
      /*sessionIdle=*/true));
  assert(!UsbMscPrompt::shouldShowOnUsbConnect(
      /*promptEnabled=*/true, /*usbConnected=*/true, /*usbConnectedLast=*/false, /*hostSupportsUsbSerial=*/true,
      /*sessionIdle=*/false));
  assert(UsbMscPrompt::shouldShowOnUsbConnect(
      /*promptEnabled=*/true, /*usbConnected=*/true, /*usbConnectedLast=*/false, /*hostSupportsUsbSerial=*/true,
      /*sessionIdle=*/true));

  std::cout << "USB MSC prompt host gating tests passed!" << std::endl;
}

int main() {
  testPathNormalisation();
  testMarkdownLimits();
  testTodoPlannerStorageSelection();
  testInputValidation();
  testFeatureCatalogApi();
  testSettingsRoundTrip();
  testSettingsTruncatedLoad();
  testPathUtilsSecurity();
  testForkDriftCoverNavigation();
  testUsbMscPromptGate();
  std::cout << "All Host Tests Passed!" << std::endl;
  return 0;
}
