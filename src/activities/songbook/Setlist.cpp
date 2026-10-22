#include "Setlist.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace songbook {

namespace {

bool endsWithIgnoreCase(const std::string_view text, const std::string_view suffix) {
  if (text.size() < suffix.size()) return false;
  const std::string_view tail = text.substr(text.size() - suffix.size());
  for (size_t i = 0; i < suffix.size(); ++i) {
    const char c = tail[i] >= 'A' && tail[i] <= 'Z' ? static_cast<char>(tail[i] - 'A' + 'a') : tail[i];
    if (c != suffix[i]) return false;
  }
  return true;
}

}  // namespace

bool hasSongExtension(const std::string_view name) {
  return endsWithIgnoreCase(name, ".cho") || endsWithIgnoreCase(name, ".chordpro") ||
         endsWithIgnoreCase(name, ".chopro") || endsWithIgnoreCase(name, ".crd");
}

bool isSafeSongPath(const std::string_view path) {
  if (path.empty() || path.size() > 255 || path.front() == '/') return false;
  size_t segmentStart = 0;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i < path.size()) {
      const auto c = static_cast<unsigned char>(path[i]);
      if (c < 0x20 || c == '\\') return false;
      if (c != '/') continue;
    }
    const std::string_view segment = path.substr(segmentStart, i - segmentStart);
    if (segment.empty() || segment == "." || segment == "..") return false;
    segmentStart = i + 1;
  }
  return true;
}

SetlistParseResult parseSetlist(const std::string_view json, Setlist& out) {
  out.title.clear();
  out.songs.clear();
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size()) != DeserializationError::Ok || !doc.is<JsonObjectConst>()) {
    return SetlistParseResult::InvalidJson;
  }
  if (const char* title = doc["title"].as<const char*>()) out.title = title;
  const JsonArrayConst songs = doc["songs"].as<JsonArrayConst>();
  if (songs.isNull()) return SetlistParseResult::InvalidJson;
  out.songs.reserve(std::min<size_t>(songs.size(), MAX_SETLIST_SONGS));
  for (const JsonVariantConst entry : songs) {
    if (out.songs.size() >= MAX_SETLIST_SONGS) break;
    const char* path = entry.as<const char*>();
    if (path && isSafeSongPath(path)) out.songs.emplace_back(path);
  }
  return out.songs.empty() ? SetlistParseResult::NoSongs : SetlistParseResult::Ok;
}

}  // namespace songbook
