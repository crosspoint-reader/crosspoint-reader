#include "ReadwiseStore.h"

#include <ObfuscationUtils.h>

namespace {
constexpr size_t MAX_TOKEN_LENGTH = 128;

// Tokens are pasted from readwise.io/access_token; drop stray whitespace.
std::string trimmed(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}
}  // namespace

void ReadwiseStore::setToken(const std::string& t) {
  token = trimmed(t);
  if (token.size() > MAX_TOKEN_LENGTH) token.resize(MAX_TOKEN_LENGTH);
}

void ReadwiseStore::setFolder(const std::string& f) {
  std::string v = trimmed(f);
  while (v.size() > 1 && v.back() == '/') v.pop_back();
  if (v.empty() || v == "/") v = "/Readwise";
  if (v.front() != '/') v.insert(v.begin(), '/');
  folder = v;
}

void ReadwiseStore::toJson(JsonDocument& doc) const {
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["sources"] = static_cast<uint8_t>(sources);
  doc["archive_finished"] = archiveFinished;
  doc["remove_archived"] = removeArchived;
  doc["max_articles"] = maxArticles;
  doc["folder"] = folder.c_str();
}

bool ReadwiseStore::fromJson(JsonVariantConst doc) {
  bool ok = false;
  bool tooLong = false;
  token = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", MAX_TOKEN_LENGTH, &ok, &tooLong);
  if (!ok || tooLong) token.clear();
  // A plain "token" (written by hand onto the SD card) wins and is
  // re-saved obfuscated.
  const char* plain = doc["token"] | "";
  if (*plain) {
    setToken(plain);
    requestResave();
  }
  const uint8_t s = doc["sources"] | static_cast<uint8_t>(ReadwiseSources::LaterShortlist);
  sources = s < static_cast<uint8_t>(ReadwiseSources::Count) ? static_cast<ReadwiseSources>(s)
                                                             : ReadwiseSources::LaterShortlist;
  archiveFinished = doc["archive_finished"] | true;
  removeArchived = doc["remove_archived"] | true;
  const uint8_t max = doc["max_articles"] | 50;
  maxArticles = max == 0 ? 50 : max;
  setFolder(doc["folder"] | "/Readwise");
  return true;
}
