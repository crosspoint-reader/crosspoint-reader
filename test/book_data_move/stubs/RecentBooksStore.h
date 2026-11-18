#pragma once

#include <string>

class RecentBooksStore {
 public:
  static RecentBooksStore& getInstance() {
    static RecentBooksStore store;
    return store;
  }
  bool removeByPath(const std::string&) { return true; }
  void updatePath(const std::string&, const std::string&, const std::string&, const std::string&) {}
};

#define RECENT_BOOKS RecentBooksStore::getInstance()
