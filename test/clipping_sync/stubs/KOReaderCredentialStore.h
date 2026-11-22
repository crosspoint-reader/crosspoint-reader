#pragma once
#include <string>
#include <vector>

enum class KOReaderServerType { CROSSPOINT, KOSYNC, OTHER };
enum class DocumentMatchMethod { FILENAME, BINARY };

struct SyncCredentials {
  struct Header {
    std::string name, value;
  };
  bool enabled = true;
  KOReaderServerType serverType = KOReaderServerType::CROSSPOINT;
  KOReaderServerType getServerType() const { return serverType; }
  DocumentMatchMethod getMatchMethod() const { return DocumentMatchMethod::FILENAME; }
  std::vector<Header> customHeaders;
  bool getSyncClippings() const { return enabled; }
  bool hasCredentials() const { return true; }
  const std::vector<Header>& getCustomHeaders() const { return customHeaders; }
  std::string username = "reader";
  std::string getUsername() const { return username; }
  std::string getMd5Password() const { return "key"; }
  std::string getBasicAuthorization() const { return "Basic cmVhZGVyOnBhc3M="; }
  std::string getBaseUrl() const { return "https://sync.test"; }
};
inline SyncCredentials syncCredentials;
#define KOREADER_STORE syncCredentials
