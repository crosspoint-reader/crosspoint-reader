#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Song.h"

namespace songbook {

enum class TextRole : uint8_t { Lyric, Chord, Label, Comment };
// Firmware maps these to the 18/16/14 pt reader fonts.
enum class FontSize : uint8_t { Large, Medium, Small };

class TextMeasurer {
 public:
  virtual ~TextMeasurer() = default;
  virtual int width(TextRole role, FontSize size, std::string_view text) const = 0;
  virtual int lineHeight(TextRole role, FontSize size) const = 0;
};

// Display strings the layout cannot take from the song file (translated by the caller).
struct SectionNames {
  const char* verse = "Verse";
  const char* chorus = "Chorus";
  const char* bridge = "Bridge";
  const char* intro = "Intro";
  const char* outro = "Outro";
  const char* tab = "Tab";
  const char* grid = "Grid";
  const char* repeat = "(repeat)";
  const char* sameChords = "(same chords)";
};

struct Viewport {
  int width = 0;
  int height = 0;
};

// One drawn text run. x is relative to its row; dy is its top within the row.
struct Piece {
  int16_t x = 0;
  int16_t dy = 0;
  // Song::text is capped below 64 KB (ChordProParser::MAX_TEXT_BYTES).
  uint16_t offset = 0;
  uint16_t length = 0;
  TextRole role = TextRole::Lyric;
  FontSize size = FontSize::Large;
  // True when the bytes live in SongLayout::ownText instead of Song::text.
  bool own = false;
};

struct Row {
  int32_t y = 0;
  int16_t x = 0;
  uint16_t height = 0;
  uint16_t page = 0;
  uint16_t firstPiece = 0;
  uint16_t pieceCount = 0;
  // Chorus rows carry a margin bar (ChordPro convention).
  bool chorusBar = false;
};

// Upper bound on a layout's tables, reserved once before building.
struct LayoutBudget {
  size_t rows = 0;
  size_t pieces = 0;
  size_t ownBytes = 0;
  size_t atoms = 0;
  size_t bytes() const;
  // Sizes of the separately reserved tables (rows, pieces, labels, atoms).
  std::array<size_t, 4> tables() const;
};

// Positioned rows for one song in one view mode. Built once per song,
// viewport, or mode change; drawing only walks the rows in view. The row,
// piece, and label tables never grow past the budget reserved at build start;
// rows beyond it are dropped and truncated() reports it.
class SongLayout {
 public:
  static constexpr int CHORUS_INDENT = 14;
  static constexpr int COLUMN_GAP = 24;

  static LayoutBudget lyricsBudget(const Song& song);
  static LayoutBudget chartBudget(const Song& song);

  // Lyrics with chords placed above the byte they anchor to. Rows are one
  // continuous strip; scrollStops() holds row-aligned tops ~1/3 screen apart.
  void buildLyrics(const Song& song, const TextMeasurer& measurer, Viewport viewport, const SectionNames& names);
  // Chords only, from the explicit chart or derived from the lyrics. Picks the
  // largest size (and one or two columns) that fits one screen, else pages.
  void buildChart(const Song& song, const TextMeasurer& measurer, Viewport viewport, const SectionNames& names);

  const std::vector<Row>& rows() const { return rowList; }
  const std::vector<Piece>& pieces() const { return pieceList; }
  const std::vector<int32_t>& scrollStops() const { return stops; }
  int32_t contentHeight() const { return height; }
  uint16_t pageCount() const { return pages; }
  uint8_t columns() const { return columnCount; }
  FontSize chartSize() const { return size; }
  bool autoChart() const { return derivedChart; }
  // True when the budget ran out and rows were dropped.
  bool truncated() const { return full; }
  std::string_view text(const Song& song, const Piece& piece) const;

  // Row-aligned scroll tops: 0, then the last row start within each third of
  // the viewport, ending at the first top that shows the final row whole.
  static void computeScrollStops(const std::vector<Row>& rows, int32_t contentHeight, int viewHeight,
                                 std::vector<int32_t>& out);

 private:
  // A lyric run that never splits across rows: up to a chord anchor or word end.
  struct Atom {
    uint16_t start;
    uint16_t end;
    int16_t chord;
    bool breakBefore;
  };
  struct PlaceState {
    int x;
    int chordEnd;
    int extent;
    // Piece a following chordless run extends, or -1; runs merge only when contiguous.
    int lastLyricPiece;
    uint32_t lastLyricEnd;
  };

  void reserve(const LayoutBudget& budget);
  void clear();
  void beginRow(int32_t y, bool chorusBar);
  void endRow(uint16_t height);
  void addPiece(int x, int dy, TextRef ref, bool own, TextRole role, FontSize pieceSize);
  TextRef addOwnText(std::string_view text);
  std::string_view refText(const Song& song, TextRef ref, bool own) const;
  void sectionLabel(const Song& song, const Section& section, int ordinal, const SectionNames& names,
                    std::string& out) const;

  int32_t layoutLyricLine(const Song& song, const SongLine& line, const TextMeasurer& measurer, int width, int indent,
                          int32_t y, bool chorus);
  int32_t layoutWrapped(std::string_view text, TextRef ref, bool own, TextRole role, FontSize pieceSize,
                        const TextMeasurer& measurer, int width, int indent, int32_t y, bool chorus);
  int32_t layoutChartChords(const Song& song, const SongLine& line, int repeatCount, const TextMeasurer& measurer,
                            FontSize pieceSize, int width, int32_t y);
  // Places atoms [first, last) from state; commit=false only measures.
  void placeAtoms(const Song& song, const SongLine& line, size_t first, size_t last, const TextMeasurer& measurer,
                  PlaceState& state, bool commit, bool& rowHasChord, bool& rowHasLyric);
  // Sets lyric offsets under the chord band and closes the row; returns its height.
  int finishLyricRow(const TextMeasurer& measurer, bool hasChord, bool hasLyric);
  // Lays the chart at one size and column width as one strip of section groups.
  void layoutChartAt(const Song& song, const TextMeasurer& measurer, FontSize pieceSize, int width,
                     const SectionNames& names, std::vector<uint32_t>& groupStarts);
  bool packColumns(const std::vector<uint32_t>& groupStarts, int columns, int columnWidth, int viewHeight);
  void paginate(const std::vector<uint32_t>& groupStarts, int viewHeight);

  std::vector<Row> rowList;
  std::vector<Piece> pieceList;
  std::vector<int32_t> stops;
  std::string ownText;
  std::vector<Atom> atoms;
  int32_t height = 0;
  int chartGap = 0;
  uint16_t pages = 1;
  uint8_t columnCount = 1;
  FontSize size = FontSize::Large;
  bool derivedChart = false;
  bool rowOpen = false;
  bool full = false;
};

}  // namespace songbook
