#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

/**
 * OAuth token state for one OPDS server, keyed by server URL.
 * accessToken/refreshToken are bearer credentials, stored XOR-obfuscated with
 * the device key (same protection as the server password). refreshUrl is the
 * token endpoint discovered from the authentication document; it lets an
 * expired access token be refreshed without a full re-login.
 */
struct OpdsTokens {
  std::string accessToken;
  std::string refreshToken;
  std::string refreshUrl;

  bool empty() const { return accessToken.empty(); }
};

/**
 * Persists per-server OAuth tokens on SD (/.crosspoint/opds_tokens.json) so the
 * device does not re-authenticate every session, and can refresh an expired
 * access token using its refresh token.
 */
class OpdsTokenStore : public PersistableStore<OpdsTokenStore> {
 private:
  struct Entry {
    std::string url;
    OpdsTokens tokens;
  };
  std::vector<Entry> entries;
  static constexpr size_t MAX_SERVERS = 8;

  OpdsTokenStore() = default;
  friend class PersistableStore<OpdsTokenStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/opds_tokens.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns the stored tokens for a server URL (empty when none).
  OpdsTokens get(const std::string& url) const;
  // Upserts the tokens for a server URL. A no-op (no SD write) when unchanged;
  // empty tokens remove the entry. Returns true on success.
  bool put(const std::string& url, const OpdsTokens& tokens);
};

#define OPDS_TOKENS OpdsTokenStore::getInstance()
