#pragma once

#include <string>
#include <string_view>

#include "ChordProParser.h"
#include "SongLayout.h"

namespace songbook_test {

// Deterministic metrics: every codepoint has a role-specific fixed advance.
class FixedMeasurer final : public songbook::TextMeasurer {
 public:
  static int scale(const songbook::FontSize size, const int value) {
    switch (size) {
      case songbook::FontSize::Large:
        return value;
      case songbook::FontSize::Medium:
        return value * 9 / 10;
      case songbook::FontSize::Small:
        return value * 8 / 10;
    }
    return value;
  }
  static int advance(const songbook::TextRole role) {
    switch (role) {
      case songbook::TextRole::Lyric:
        return 10;
      case songbook::TextRole::Chord:
        return 12;
      case songbook::TextRole::Label:
        return 8;
      case songbook::TextRole::Comment:
        return 9;
    }
    return 10;
  }
  static int codepoints(const std::string_view text) {
    int count = 0;
    for (const char c : text) count += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return count;
  }
  int width(const songbook::TextRole role, const songbook::FontSize size, const std::string_view text) const override {
    return codepoints(text) * scale(size, advance(role));
  }
  int lineHeight(const songbook::TextRole role, const songbook::FontSize size) const override {
    switch (role) {
      case songbook::TextRole::Lyric:
        return scale(size, 30);
      case songbook::TextRole::Chord:
        return scale(size, 26);
      case songbook::TextRole::Label:
        return scale(size, 20);
      case songbook::TextRole::Comment:
        return scale(size, 24);
    }
    return 30;
  }
};

inline songbook::Song parse(const std::string& source, const bool metadataOnly = false) {
  songbook::Song song;
  songbook::ChordProParser parser(song, metadataOnly);
  size_t start = 0;
  while (start <= source.size()) {
    size_t end = source.find('\n', start);
    if (end == std::string::npos) end = source.size();
    if (!parser.feedLine(std::string_view(source).substr(start, end - start))) break;
    start = end + 1;
  }
  parser.finish();
  return song;
}

}  // namespace songbook_test
