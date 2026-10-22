#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Song.h"

namespace songbook {

// Line-at-a-time parser for the ChordPro subset the Songbook renders:
// metadata, verse/chorus/bridge/tab/grid and generic start_of_X sections,
// {chorus} repeats, comments, and inline [chord] anchors. Unknown directives
// are skipped. The custom {start_of_x_chart} block (ChordPro's x_ namespace)
// holds an explicit "Label: chords" summary for the chords-only view.
class ChordProParser {
 public:
  // Bounds for a corrupt or oversized file; content past them is dropped.
  static constexpr size_t MAX_LINE_BYTES = 1024;
  static constexpr size_t MAX_TEXT_BYTES = 48 * 1024;
  static constexpr size_t MAX_LINES = 2000;
  static constexpr size_t MAX_CHORDS = 4000;
  static constexpr size_t MAX_SECTIONS = 200;
  static constexpr size_t MAX_CHORDS_PER_LINE = 32;
  // An explicit chart is a short summary; more rows than this are dropped.
  static constexpr size_t MAX_CHART_ROWS = 64;

  // metadataOnly stops at the first body line so song lists read only headers.
  explicit ChordProParser(Song& song, bool metadataOnly = false);

  // Feeds one line without its terminator. Returns false when the caller can
  // stop reading (metadata-only parsing reached the song body).
  bool feedLine(std::string_view line);
  void finish();

  bool truncated() const { return truncatedContent; }
  uint16_t skippedDirectives() const { return skipped; }

  // Seconds from "210", "3:30", or "1:02:03"; 0 when unparseable.
  static uint32_t parseDuration(std::string_view value);

 private:
  struct PendingChord {
    uint16_t nameStart;
    uint8_t nameLength;
    uint16_t position;
  };

  void handleDirective(std::string_view name, std::string_view value);
  void handleMeta(std::string_view name, std::string_view value);
  void handleLyricLine(std::string_view line);
  void handleChartLine(std::string_view line);
  void addTextLine(LineKind kind, std::string_view text);
  void openSection(SectionType type, std::string_view label);
  void closeSection();
  bool ensureImplicitSection();
  void addRepeat(std::string_view label);
  bool appendText(std::string_view bytes, TextRef& ref);

  Song& song;
  const bool metadataOnly;
  bool sectionOpen = false;
  bool inChart = false;
  bool firstLine = true;
  bool truncatedContent = false;
  uint16_t skipped = 0;
  PendingChord pending[MAX_CHORDS_PER_LINE]{};
};

}  // namespace songbook
