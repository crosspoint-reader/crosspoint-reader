#include "WifiCredentialStore.h"

#include <CredentialIntegrity.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>

void WifiCredentialStore::toJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  doc["lastConnectedSsid"] = lastConnectedSsid;

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : credentials) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
    // XOR and base64 must round-trip to exactly this many bytes. Keeping the
    // original length lets the loader reject a decodable-but-corrupted value
    // instead of silently trying a different password.
    obj["password_len"] = cred.password.size();
    obj["password_crc32"] = credential_integrity::crc32(cred.password);
  }
}

bool WifiCredentialStore::fromJson(JsonVariantConst doc) {
  std::lock_guard<std::mutex> lock(credentialMutex);
  lastConnectedSsid = doc["lastConnectedSsid"] | "";

  // Tolerate a missing/invalid 'credentials' key (treat as empty list); only
  // a JSON parse error is fatal. A null JsonArray iterates zero times.
  credentials.clear();
  JsonArrayConst arr = doc["credentials"].as<JsonArrayConst>();
  credentials.reserve(std::min(arr.size(), MAX_NETWORKS));
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (credentials.size() >= MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | "";

    const JsonVariantConst passwordLength = obj["password_len"];
    const bool hasPasswordLength = !passwordLength.isNull();
    size_t expectedLength = 0;
    if (hasPasswordLength) {
      if (!passwordLength.is<size_t>()) {
        LOG_ERR("WCS", "Discarding corrupted password for %s (invalid length)", cred.ssid.c_str());
        needsResave = true;
        continue;
      }
      expectedLength = passwordLength.as<size_t>();
      if (expectedLength > MAX_PASSWORD_LENGTH) {
        LOG_ERR("WCS", "Discarding oversized password for %s (%zu bytes)", cred.ssid.c_str(), expectedLength);
        needsResave = true;
        continue;
      }
    }

    bool passwordValid = false;
    cred.password = extractPassword(obj, needsResave, MAX_PASSWORD_LENGTH, passwordValid);
    if (!passwordValid) {
      LOG_ERR("WCS", "Discarding oversized password for %s", cred.ssid.c_str());
      needsResave = true;
      continue;
    }

    bool integrityValid = true;
    if (hasPasswordLength) {
      if (cred.password.size() != expectedLength) {
        LOG_ERR("WCS", "Discarding corrupted password for %s (expected %zu bytes, decoded %zu)", cred.ssid.c_str(),
                expectedLength, cred.password.size());
        integrityValid = false;
      }
    } else {
      // Upgrade existing JSON after it has loaded successfully.
      needsResave = true;
    }

    const JsonVariantConst checksum = obj["password_crc32"];
    if (checksum.is<uint32_t>()) {
      const uint32_t expectedCrc32 = checksum.as<uint32_t>();
      if (credential_integrity::crc32(cred.password) != expectedCrc32) {
        LOG_ERR("WCS", "Discarding corrupted password for %s (checksum mismatch)", cred.ssid.c_str());
        integrityValid = false;
      }
    } else if (checksum.isNull()) {
      // password_crc32 was added after password_len; accept and upgrade files
      // written by older firmware.
      needsResave = true;
    } else {
      LOG_ERR("WCS", "Discarding corrupted password for %s (invalid checksum)", cred.ssid.c_str());
      integrityValid = false;
    }

    if (!integrityValid) {
      needsResave = true;
      continue;
    }
    credentials.push_back(cred);
  }

  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", credentials.size());

  if (needsResave) {
    LOG_DBG("WCS", "Resaving JSON with obfuscated passwords");
    requestResave();
  }

  return true;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);

    // Check if this SSID already exists and update it
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
    if (cred != credentials.end()) {
      cred->password = password;
      LOG_DBG("WCS", "Updated credentials for: %s", ssid.c_str());
    } else {
      // Check if we've reached the limit
      if (credentials.size() >= MAX_NETWORKS) {
        LOG_DBG("WCS", "Cannot add more networks, limit of %zu reached", MAX_NETWORKS);
        return false;
      }

      // Add new credential
      credentials.push_back({ssid, password});
      LOG_DBG("WCS", "Added credentials for: %s", ssid.c_str());
    }
  }
  return saveToFile();
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
    if (cred == credentials.end()) {
      return false;  // Not found
    }
    credentials.erase(cred);
    LOG_DBG("WCS", "Removed credentials for: %s", ssid.c_str());
    if (ssid == lastConnectedSsid) lastConnectedSsid.clear();
  }
  return saveToFile();
}

std::optional<WifiCredential> WifiCredentialStore::findCredential(const std::string& ssid) const {
  std::lock_guard<std::mutex> lock(credentialMutex);

  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });

  if (cred != credentials.end()) {
    return *cred;
  }

  return std::nullopt;
}

std::optional<WifiCredential> WifiCredentialStore::getCredentialAt(const size_t index) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  if (index >= credentials.size()) return std::nullopt;
  return credentials[index];
}

std::optional<std::string> WifiCredentialStore::getSsidAt(const size_t index) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  if (index >= credentials.size()) return std::nullopt;
  return credentials[index].ssid;
}

size_t WifiCredentialStore::getCredentialCount() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return credentials.size();
}

std::vector<WifiCredentialSummary> WifiCredentialStore::getCredentialSummaries() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  std::vector<WifiCredentialSummary> summaries;
  summaries.reserve(credentials.size());
  for (const auto& credential : credentials) {
    summaries.push_back({credential.ssid, !credential.password.empty(), credential.ssid == lastConnectedSsid});
  }
  return summaries;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return find_if(credentials.begin(), credentials.end(),
                 [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; }) != credentials.end();
}

void WifiCredentialStore::setLastConnectedSsid(const std::string& ssid) {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (lastConnectedSsid == ssid) return;
    lastConnectedSsid = ssid;
  }
  saveToFile();
}

std::string WifiCredentialStore::getLastConnectedSsid() const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  return lastConnectedSsid;
}

void WifiCredentialStore::clearLastConnectedSsid() {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (lastConnectedSsid.empty()) return;
    lastConnectedSsid.clear();
  }
  saveToFile();
}

void WifiCredentialStore::clearAll() {
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    credentials.clear();
    lastConnectedSsid.clear();
  }
  saveToFile();
  LOG_DBG("WCS", "Cleared all WiFi credentials");
}
