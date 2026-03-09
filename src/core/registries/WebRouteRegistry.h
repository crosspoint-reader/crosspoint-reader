#pragma once

#include <cstddef>

class WebServer;

namespace core {

struct WebRouteEntry {
  const char* routeId;                     // unique identifier string e.g. anki_plugin
  bool (*shouldRegister)();                // returns false if feature disabled
  void (*mountRoutes)(WebServer* server);  // mounts all HTTP routes for this feature
};

class WebRouteRegistry {
 public:
  static constexpr int kMaxEntries = 24;

  static void add(const WebRouteEntry& entry) {
    if (count >= kMaxEntries) {
      return;
    }

    entries[count++] = entry;
  }

  static bool shouldRegister(const char* routeId) {
    const WebRouteEntry* entry = find(routeId);
    return entry != nullptr && entry->shouldRegister != nullptr && entry->shouldRegister();
  }

  static void mountAll(WebServer* server) {
    if (server == nullptr) {
      return;
    }

    for (int i = 0; i < count; ++i) {
      if (entries[i].shouldRegister != nullptr && entries[i].shouldRegister() && entries[i].mountRoutes != nullptr) {
        entries[i].mountRoutes(server);
      }
    }
  }

 private:
  static const WebRouteEntry* find(const char* routeId) {
    if (routeId == nullptr) {
      return nullptr;
    }

    for (int i = 0; i < count; ++i) {
      if (cStringsEqual(entries[i].routeId, routeId)) {
        return &entries[i];
      }
    }

    return nullptr;
  }

  static bool cStringsEqual(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) {
      return left == right;
    }

    while (*left != '\0' && *right != '\0') {
      if (*left != *right) {
        return false;
      }

      ++left;
      ++right;
    }

    return *left == *right;
  }

  inline static WebRouteEntry entries[kMaxEntries] = {};
  inline static int count = 0;
};

}  // namespace core
