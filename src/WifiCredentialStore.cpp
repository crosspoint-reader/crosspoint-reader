#include "WifiCredentialStore.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <WiFi.h>
#include <freertos/task.h>

#include "activities/ActivityWithSubactivity.h"
#include "activities/network/WifiSelectionActivity.h"

// Initialize the static instance
WifiCredentialStore WifiCredentialStore::instance;

namespace {
// File format version
constexpr uint8_t WIFI_FILE_VERSION = 1;

// WiFi credentials file path
constexpr char WIFI_FILE[] = "/.crosspoint/wifi.bin";

// Obfuscation key - "CrossPoint" in ASCII
// This is NOT cryptographic security, just prevents casual file reading
constexpr uint8_t OBFUSCATION_KEY[] = {0x43, 0x72, 0x6F, 0x73, 0x73, 0x50, 0x6F, 0x69, 0x6E, 0x74};
constexpr size_t KEY_LENGTH = sizeof(OBFUSCATION_KEY);
}  // namespace

void WifiCredentialStore::obfuscate(std::string& data) const {
  Serial.printf("[%lu] [WCS] Obfuscating/deobfuscating %zu bytes\n", millis(), data.size());
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= OBFUSCATION_KEY[i % KEY_LENGTH];
  }
}

bool WifiCredentialStore::saveToFile() const {
  // Make sure the directory exists
  SdMan.mkdir("/.crosspoint");

  FsFile file;
  if (!SdMan.openFileForWrite("WCS", WIFI_FILE, file)) {
    return false;
  }

  // Write header
  serialization::writePod(file, WIFI_FILE_VERSION);
  serialization::writePod(file, static_cast<uint8_t>(credentials.size()));

  // Write each credential
  for (const auto& cred : credentials) {
    // Write SSID (plaintext - not sensitive)
    serialization::writeString(file, cred.ssid);
    Serial.printf("[%lu] [WCS] Saving SSID: %s, password length: %zu\n", millis(), cred.ssid.c_str(),
                  cred.password.size());

    // Write password (obfuscated)
    std::string obfuscatedPwd = cred.password;
    obfuscate(obfuscatedPwd);
    serialization::writeString(file, obfuscatedPwd);
  }

  // Write default SSID
  serialization::writeString(file, defaultSSID);
  Serial.printf("[%lu] [WCS] Saving default SSID: %s\n", millis(), defaultSSID.c_str());

  file.close();
  Serial.printf("[%lu] [WCS] Saved %zu WiFi credentials to file\n", millis(), credentials.size());
  return true;
}

bool WifiCredentialStore::loadFromFile() {
  FsFile file;
  if (!SdMan.openFileForRead("WCS", WIFI_FILE, file)) {
    return false;
  }

  // Read and verify version
  uint8_t version;
  serialization::readPod(file, version);
  if (version != WIFI_FILE_VERSION) {
    Serial.printf("[%lu] [WCS] Unknown file version: %u\n", millis(), version);
    file.close();
    return false;
  }

  // Read credential count
  uint8_t count;
  serialization::readPod(file, count);

  // Read credentials
  credentials.clear();
  for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
    WifiCredential cred;

    // Read SSID
    serialization::readString(file, cred.ssid);

    // Read and deobfuscate password
    serialization::readString(file, cred.password);
    Serial.printf("[%lu] [WCS] Loaded SSID: %s, obfuscated password length: %zu\n", millis(), cred.ssid.c_str(),
                  cred.password.size());
    obfuscate(cred.password);  // XOR is symmetric, so same function deobfuscates
    Serial.printf("[%lu] [WCS] After deobfuscation, password length: %zu\n", millis(), cred.password.size());

    credentials.push_back(cred);
  }

  // Try to read default SSID if it exists
  defaultSSID.clear();
  if (file.available() >= 4) {
    const uint32_t posBefore = file.position();
    uint32_t len = 0;
    serialization::readPod(file, len);

    if (file.available() >= len && len <= 64) {
      defaultSSID.resize(len);
      const size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(&defaultSSID[0]), len);
      if (bytesRead == len) {
        Serial.printf("[%lu] [WCS] Loaded default SSID: %s\n", millis(), defaultSSID.c_str());
      } else {
        file.seek(posBefore);
        defaultSSID.clear();
      }
    } else {
      file.seek(posBefore);
    }
  }

  file.close();
  Serial.printf("[%lu] [WCS] Loaded %zu WiFi credentials from file\n", millis(), credentials.size());
  return true;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  // Check if this SSID already exists and update it
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != credentials.end()) {
    cred->password = password;
    Serial.printf("[%lu] [WCS] Updated credentials for: %s\n", millis(), ssid.c_str());
    return saveToFile();
  }

  // Check if we've reached the limit
  if (credentials.size() >= MAX_NETWORKS) {
    Serial.printf("[%lu] [WCS] Cannot add more networks, limit of %zu reached\n", millis(), MAX_NETWORKS);
    return false;
  }

  // Add new credential
  credentials.push_back({ssid, password});
  Serial.printf("[%lu] [WCS] Added credentials for: %s\n", millis(), ssid.c_str());
  return saveToFile();
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != credentials.end()) {
    credentials.erase(cred);
    if (defaultSSID == ssid) {
      defaultSSID.clear();
    }
    Serial.printf("[%lu] [WCS] Removed credentials for: %s\n", millis(), ssid.c_str());
    return saveToFile();
  }
  return false;  // Not found
}

const WifiCredential* WifiCredentialStore::findCredential(const std::string& ssid) const {
  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });

  if (cred != credentials.end()) {
    return &*cred;
  }

  return nullptr;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const { return findCredential(ssid) != nullptr; }

void WifiCredentialStore::clearAll() {
  credentials.clear();
  defaultSSID.clear();
  saveToFile();
  Serial.printf("[%lu] [WCS] Cleared all WiFi credentials\n", millis());
}

void WifiCredentialStore::setDefaultSSID(const std::string& ssid) {
  defaultSSID = ssid;
  saveToFile();
  Serial.printf("[%lu] [WCS] Set default SSID: %s\n", millis(), ssid.c_str());
}

bool WifiCredentialStore::connectToDefaultWifi(int timeoutMs) const {
  if (defaultSSID.empty()) {
    Serial.printf("[%lu] [WCS] No default SSID set\n", millis());
    return false;
  }

  const auto* cred = findCredential(defaultSSID);
  if (!cred) {
    return false;
  }

  // Quick check: scan to see if the SSID is available before attempting connection
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  delay(100);
  
  Serial.printf("[%lu] [WCS] Scanning for SSID: %s\n", millis(), defaultSSID.c_str());
  WiFi.scanNetworks(false);
  
  const unsigned long scanStart = millis();
  int16_t scanResult = WiFi.scanComplete();
  while (scanResult == WIFI_SCAN_RUNNING && millis() - scanStart < 3000) {
    delay(100);
    scanResult = WiFi.scanComplete();
  }
  
  if (scanResult > 0) {
    bool ssidFound = false;
    for (int i = 0; i < scanResult; i++) {
      std::string scannedSSID = WiFi.SSID(i).c_str();
      if (scannedSSID == defaultSSID) {
        ssidFound = true;
        break;
      }
    }
    
    WiFi.scanDelete();
    
    if (!ssidFound) {
      Serial.printf("[%lu] [WCS] SSID not found in scan results, skipping connection attempt\n", millis());
      return false;
    }
  } else {
    WiFi.scanDelete();
    return false;
  }

  WiFi.begin(defaultSSID.c_str(), cred->password.c_str());

  Serial.printf("[%lu] [WCS] Connecting to default WiFi: %s\n", millis(), defaultSSID.c_str());
  const unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < static_cast<unsigned long>(timeoutMs)) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[%lu] [WCS] Connected to default WiFi: %s (IP: %s)\n", millis(), defaultSSID.c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.printf("[%lu] [WCS] Failed to connect to default WiFi: %s\n", millis(), defaultSSID.c_str());
    return false;
  }
}

void WifiCredentialStore::ensureWifiConnected(ActivityWithSubactivity& activity, GfxRenderer& renderer,
                                              MappedInputManager& mappedInput, const std::function<void()>& onSuccess,
                                              const std::function<void()>& onCancel, int timeoutMs) {

  if (WiFi.status() == WL_CONNECTED) {
    onSuccess();
    return;
  }

  // Try to connect using default WiFi
  WifiCredentialStore::getInstance().loadFromFile();
  if (WifiCredentialStore::getInstance().connectToDefaultWifi(timeoutMs)) {
    Serial.printf("[%lu] [WCS] Auto-connected to WiFi\n", millis());
    onSuccess();
    return;
  }

  // Auto-connect failed - show WiFi selection list
  Serial.printf("[%lu] [WCS] Auto-connect failed, showing WiFi selection\n", millis());
  activity.enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [&activity, onSuccess, onCancel](bool connected) {
    activity.exitActivity();
    if (connected) {
      onSuccess();
    } else {
      onCancel();
    }
  }));
}
