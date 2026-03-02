// Link-time stubs for native unit tests.
// Provides minimal implementations of SDK/device functions that are referenced
// by the code under test but never actually called during the test scenarios.
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include <I18n.h>

// Static singleton instances (declared extern in the headers)
CrossPointSettings CrossPointSettings::instance;
CrossPointState CrossPointState::instance;

// CrossPointSettings device-only methods — not exercised by native tests
bool CrossPointSettings::saveToFile() const { return false; }
bool CrossPointSettings::loadFromFile() { return false; }
bool CrossPointSettings::loadFromBinaryFile() { return false; }
void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings&) {}

// I18n stub — loadSettings/saveSettings iterate over StrId values but never
// call get() for the native test cases, so this is a no-op singleton.
// Use static-local to avoid calling the private I18n() constructor directly.
I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}
const char* I18n::get(StrId) const { return ""; }
void I18n::setLanguage(Language) {}
const char* I18n::getLanguageName(Language) const { return ""; }
void I18n::saveSettings() {}
void I18n::loadSettings() {}
const char* I18n::getCharacterSet(Language) { return ""; }

// String table data referenced via extern in I18nKeys.h — never dereferenced
// by JsonSettingsIO, but must satisfy the linker.
namespace i18n_strings {
const char* const STRINGS_EN[] = {nullptr};
const char* const STRINGS_ES[] = {nullptr};
const char* const STRINGS_FR[] = {nullptr};
const char* const STRINGS_DE[] = {nullptr};
const char* const STRINGS_CS[] = {nullptr};
const char* const STRINGS_PO[] = {nullptr};
const char* const STRINGS_RU[] = {nullptr};
const char* const STRINGS_SV[] = {nullptr};
const char* const STRINGS_RO[] = {nullptr};
const char* const STRINGS_CA[] = {nullptr};
const char* const STRINGS_UK[] = {nullptr};
const char* const STRINGS_BE[] = {nullptr};
const char* const STRINGS_IT[] = {nullptr};
const char* const STRINGS_PL[] = {nullptr};
const char* const STRINGS_FI[] = {nullptr};
const char* const STRINGS_DA[] = {nullptr};
const char* const STRINGS_NL[] = {nullptr};
}  // namespace i18n_strings

const char* const LANGUAGE_NAMES[] = {nullptr};
const char* const CHARACTER_SETS[] = {nullptr};

// WifiCredentialStore stubs — SD card I/O and binary migration not exercised in native tests
WifiCredentialStore WifiCredentialStore::instance;
const std::string& WifiCredentialStore::getLastConnectedSsid() const { return lastConnectedSsid; }
bool WifiCredentialStore::saveToFile() const { return false; }
bool WifiCredentialStore::loadFromFile() { return false; }
bool WifiCredentialStore::loadFromBinaryFile() { return false; }
bool WifiCredentialStore::addCredential(const std::string&, const std::string&) { return false; }
bool WifiCredentialStore::removeCredential(const std::string&) { return false; }
const WifiCredential* WifiCredentialStore::findCredential(const std::string&) const { return nullptr; }
bool WifiCredentialStore::hasSavedCredential(const std::string&) const { return false; }
void WifiCredentialStore::setLastConnectedSsid(const std::string&) {}
void WifiCredentialStore::clearLastConnectedSsid() {}
void WifiCredentialStore::clearAll() {}

// KOReaderCredentialStore stubs — device-only (MD5, HTTP, binary migration not available natively)
KOReaderCredentialStore KOReaderCredentialStore::instance;
bool KOReaderCredentialStore::saveToFile() const { return false; }
bool KOReaderCredentialStore::loadFromFile() { return false; }
bool KOReaderCredentialStore::loadFromBinaryFile() { return false; }
void KOReaderCredentialStore::setCredentials(const std::string&, const std::string&) {}
std::string KOReaderCredentialStore::getMd5Password() const { return ""; }
bool KOReaderCredentialStore::hasCredentials() const { return false; }
void KOReaderCredentialStore::clearCredentials() {}
void KOReaderCredentialStore::setServerUrl(const std::string&) {}
std::string KOReaderCredentialStore::getBaseUrl() const { return ""; }
void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod) {}

// RecentBooksStore stubs — Epub/Xtc libraries not available natively
RecentBooksStore RecentBooksStore::instance;
bool RecentBooksStore::saveToFile() const { return false; }
bool RecentBooksStore::loadFromFile() { return false; }
bool RecentBooksStore::loadFromBinaryFile() { return false; }
void RecentBooksStore::addBook(const std::string&, const std::string&, const std::string&,
                               const std::string&) {}
void RecentBooksStore::updateBook(const std::string&, const std::string&, const std::string&,
                                  const std::string&) {}
RecentBook RecentBooksStore::getDataFromBook(std::string) const { return {}; }
