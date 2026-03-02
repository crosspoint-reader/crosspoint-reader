/**
 * Native unit tests for JsonSettingsIO — settings round-trip and defaults.
 * Run with: pio test -e native
 */
#include <unity.h>
#include "CrossPointSettings.h"
#include "JsonSettingsIO.h"

void setUp() {}
void tearDown() {}

// Parsing a minimal JSON object should not crash and should leave unset fields
// at their defaults.
void test_load_empty_json_object() {
    CrossPointSettings s;
    bool needsResave = false;
    bool ok = JsonSettingsIO::loadSettings(s, "{}", &needsResave);
    TEST_ASSERT_TRUE(ok);
    // needsResave should be true because all keys were missing
    TEST_ASSERT_TRUE(needsResave);
}

// A fully-absent key should leave the default value intact.
void test_default_font_family() {
    CrossPointSettings s;
    JsonSettingsIO::loadSettings(s, "{}", nullptr);
    TEST_ASSERT_EQUAL(CrossPointSettings::BOOKERLY, s.fontFamily);
}

// Danger Zone disabled by default.
void test_default_danger_zone_disabled() {
    CrossPointSettings s;
    JsonSettingsIO::loadSettings(s, "{}", nullptr);
    TEST_ASSERT_FALSE(s.dangerZoneEnabled);
}

// Explicit dangerZoneEnabled=true should round-trip.
void test_danger_zone_enabled_round_trip() {
    CrossPointSettings s;
    JsonSettingsIO::loadSettings(s, R"({"dangerZoneEnabled":true})", nullptr);
    TEST_ASSERT_TRUE(s.dangerZoneEnabled);
}

// Unknown keys should be silently ignored (no crash, no error).
void test_unknown_keys_ignored() {
    CrossPointSettings s;
    bool ok = JsonSettingsIO::loadSettings(
        s, R"({"unknownFutureKey":42,"anotherKey":"hello"})", nullptr);
    TEST_ASSERT_TRUE(ok);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_load_empty_json_object);
    RUN_TEST(test_default_font_family);
    RUN_TEST(test_default_danger_zone_disabled);
    RUN_TEST(test_danger_zone_enabled_round_trip);
    RUN_TEST(test_unknown_keys_ignored);
    return UNITY_END();
}
