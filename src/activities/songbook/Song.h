#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace songbook {

enum class SectionType : uint8_t { None, Verse, Chorus, Bridge, Intro, Outro, Tab, Grid, Other };

// Byte range into Song::text.
struct TextRef {
  uint32_t offset = 0;
  uint16_t length = 0;
};

struct ChordAnchor {
  TextRef name;
  // Byte offset into the owning line's lyric where the chord is played.
  uint16_t position = 0;
};

enum class LineKind : uint8_t { Lyrics, Comment, Tab, Empty };

struct SongLine {
  LineKind kind = LineKind::Lyrics;
  uint16_t section = 0;
  TextRef lyric;
  uint16_t firstChord = 0;
  uint16_t chordCount = 0;
};

struct Section {
  SectionType type = SectionType::None;
  // Explicit label from the file; empty means the display derives one from type.
  TextRef label;
  uint16_t firstLine = 0;
  uint16_t lineCount = 0;
  // A {chorus} reference owns no lines; repeatOf is the chorus it repeats, or -1.
  bool repeat = false;
  int16_t repeatOf = -1;
};

// One row of an explicit {start_of_x_chart} block: "Label: chords".
struct ChartRow {
  TextRef label;
  TextRef chords;
};

struct SongMeta {
  std::string title;
  std::string artist;
  std::string key;
  std::string capo;
  uint16_t tempo = 0;
  // 0 when the file gives no usable duration.
  uint32_t durationSec = 0;
};

// One parsed song. All lyric, chord, and label bytes share one pool so a song
// costs a few allocations regardless of its line count.
struct Song {
  SongMeta meta;
  std::string text;
  std::vector<SongLine> lines;
  std::vector<ChordAnchor> chords;
  std::vector<Section> sections;
  std::vector<ChartRow> chart;

  std::string_view view(const TextRef ref) const { return {text.data() + ref.offset, ref.length}; }
  void clear() {
    meta = SongMeta{};
    text.clear();
    lines.clear();
    chords.clear();
    sections.clear();
    chart.clear();
  }
};

}  // namespace songbook
