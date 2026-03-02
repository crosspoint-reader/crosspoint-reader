/**
 * Native unit tests for JsonSettingsIO — settings and WiFi credential round-trips.
 * Run with: pio test -e native
 */
#include <unity.h>
#include "CrossPointSettings.h"
#include "JsonSettingsIO.h"
#include "WifiCredentialStore.h"

void setUp() {}
void tearDown() {}

// ---- Settings tests ----

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

// Explicit dangerZoneEnabled=true should round-trip (JSON boolean → uint8_t).
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

// ---- WiFi credential tests ----

// Empty credentials JSON should load without error.
void test_wifi_load_empty_credentials() {
    WifiCredentialStore& store = WifiCredentialStore::getInstance();
    bool ok = JsonSettingsIO::loadWifi(store, R"({"credentials":[],"lastConnectedSsid":""})", nullptr);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(0, store.getCredentials().size());
}

// A credential stored with password_obf should round-trip through loadWifi.
// The native obfuscation stub is identity (no XOR/base64), so password_obf == plaintext.
void test_wifi_credentials_load() {
    WifiCredentialStore& store = WifiCredentialStore::getInstance();
    const char* json = R"({"credentials":[{"ssid":"HomeNet","password_obf":"s3cr3t"}],"lastConnectedSsid":"HomeNet"})";
    bool ok = JsonSettingsIO::loadWifi(store, json, nullptr);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, store.getCredentials().size());
    TEST_ASSERT_EQUAL_STRING("HomeNet", store.getCredentials()[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("s3cr3t",  store.getCredentials()[0].password.c_str());
    TEST_ASSERT_EQUAL_STRING("HomeNet", store.getLastConnectedSsid().c_str());
}

// Multiple credentials should all load.
void test_wifi_multiple_credentials() {
    WifiCredentialStore& store = WifiCredentialStore::getInstance();
    const char* json = R"({"credentials":[
        {"ssid":"Net1","password_obf":"pass1"},
        {"ssid":"Net2","password_obf":"pass2"}
    ],"lastConnectedSsid":"Net2"})";
    bool ok = JsonSettingsIO::loadWifi(store, json, nullptr);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(2, store.getCredentials().size());
    TEST_ASSERT_EQUAL_STRING("Net1", store.getCredentials()[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Net2", store.getCredentials()[1].ssid.c_str());
}

// Legacy plain-text password (no password_obf) should load and flag needsResave.
void test_wifi_plaintext_password_triggers_resave() {
    WifiCredentialStore& store = WifiCredentialStore::getInstance();
    const char* json = R"({"credentials":[{"ssid":"OldNet","password":"oldpass"}],"lastConnectedSsid":""})";
    bool needsResave = false;
    bool ok = JsonSettingsIO::loadWifi(store, json, &needsResave);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(needsResave);
    TEST_ASSERT_EQUAL(1, store.getCredentials().size());
    TEST_ASSERT_EQUAL_STRING("oldpass", store.getCredentials()[0].password.c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_load_empty_json_object);
    RUN_TEST(test_default_font_family);
    RUN_TEST(test_default_danger_zone_disabled);
    RUN_TEST(test_danger_zone_enabled_round_trip);
    RUN_TEST(test_unknown_keys_ignored);
    RUN_TEST(test_wifi_load_empty_credentials);
    RUN_TEST(test_wifi_credentials_load);
    RUN_TEST(test_wifi_multiple_credentials);
    RUN_TEST(test_wifi_plaintext_password_triggers_resave);
    return UNITY_END();
}
