#pragma once
#include <string>
#include <vector>

struct PasswordEntry {
  std::string site;
  std::string username;
  std::string password;
};

class PasswordStore {
 private:
  static PasswordStore instance;
  std::vector<PasswordEntry> entries;

  static constexpr size_t MAX_ENTRIES = 50;

  PasswordStore() = default;

 public:
  PasswordStore(const PasswordStore&) = delete;
  PasswordStore& operator=(const PasswordStore&) = delete;

  static PasswordStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool addEntry(const std::string& site, const std::string& username, const std::string& password);
  bool removeEntry(size_t index);

  const std::vector<PasswordEntry>& getEntries() const { return entries; }
  size_t size() const { return entries.size(); }
};

#define PASSWORD_STORE PasswordStore::getInstance()
