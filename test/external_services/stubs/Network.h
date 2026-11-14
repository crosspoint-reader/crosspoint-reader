#pragma once

#include <string>
#include <unordered_map>

#include "WiFi.h"

class FakeNetworkClass {
 public:
  int hostByName(const char* host, IPAddress& address) {
    ++lookups;
    const auto found = answers.find(host ? host : "");
    if (found == answers.end()) return 0;
    address = found->second;
    return 1;
  }

  std::unordered_map<std::string, IPAddress> answers;
  int lookups = 0;
};

inline FakeNetworkClass Network;
