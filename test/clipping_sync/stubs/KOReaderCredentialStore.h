#pragma once
#include <string>
#include <vector>

struct SyncCredentials {
  struct Header {
    std::string name, value;
  };
  bool enabled = true;
  bool getSyncClippings() const { return enabled; }
  bool hasCredentials() const { return true; }
  std::vector<Header> getCustomHeaders() const { return {}; }
  std::string getUsername() const { return "reader"; }
  std::string getMd5Password() const { return "key"; }
  std::string getBaseUrl() const { return "https://sync.test"; }
};
inline SyncCredentials syncCredentials;
#define KOREADER_STORE syncCredentials
